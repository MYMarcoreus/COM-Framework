#pragma once

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>

#include "Async/AsyncExecutor.h"

// ====================================================================
// 无栈协程（CCoroutine）—— 阶段一：基于 CAsyncExecutor + CTask 的
// Duff's device 状态机。
//
// 核心语义（与 CTask 的 Option 风格一致）：
//  - 协程体用 CO_BEGIN / CO_AWAIT / CO_RETURN / CO_END 宏写成顺序代码；
//  - CO_AWAIT 挂起（return 让出线程，线程回线程池），任务有值/无值后由
//    执行器投递 Resume 继续；
//  - await 到无值（None / 异常）→ 协程终止（原因透传，与 CTask 链一致）；
//  - 生命周期：绑定执行器句柄（CExecutorHandle），执行器析构/Stop 后
//    已挂起协程安全以 kStopped 终止，不悬垂。
//
// 约束（无栈协程固有）：
//  - 跨 await 的变量必须存放为派生类成员（帧），不能用函数内局部变量
//    （局部变量声明会与 switch-case 恢复点冲突）；
//  - 每个协程宏独占一行（__LINE__ 作恢复点标签，同一行两个宏会冲突）；
//  - CoStart 返回的 shared_ptr 必须被持有直到完成（Resume 访问协程对象）。
// ====================================================================

namespace common {
namespace async {

namespace detail {

/// @brief 类型擦除基类：await 返回值槽（避免基类按具体类型存储）。
struct CValueHolder
{
    virtual ~CValueHolder() {}
};

/// @brief await 返回值槽（非 void：值 + 完成标志 + 终止原因）。
template <typename U>
struct CValueHolderImpl : CValueHolder
{
    U value;
    bool bHasValue;
    CTaskEndReason reason;
    CValueHolderImpl() : value(), bHasValue(false), reason(kEndNone) {}
};

/// @brief await 返回值槽（void 特化：仅完成标志 + 终止原因）。
template <>
struct CValueHolderImpl<void> : CValueHolder
{
    bool bHasValue;
    CTaskEndReason reason;
    CValueHolderImpl() : bHasValue(false), reason(kEndNone) {}
};

/// @brief 从任务类型推导值类型（CTask<U> → U；CTask<void> → void）。
template <typename TTask>
struct CCoroutineValue
{
    using type = typename TaskTraits<TTask>::ValueType;
};

} // namespace detail

/// @brief 无栈协程基类（Option 风格，基于 CAsyncExecutor + CTask）。
///
/// 派生类实现协程体 Run()，用 CO_BEGIN / CO_AWAIT / CO_RETURN / CO_END
/// 宏写成顺序代码；宏展开为状态机（Duff's device）：await 挂起（return
/// 让出线程）、值到后由执行器投递 Resume 继续。
///
/// 跨 await 的变量必须存放为派生类成员（帧），不能用函数内局部变量。
/// 结果经 Get() 获取（与 CTask::Get 语义一致，不抛异常）。
///
/// @tparam TValue 协程产出的值类型（void 表示无返回值协程）。
template <typename TValue>
class CCoroutine
{
public:
    /// @brief 创建协程（未绑定执行器；经 CAsyncExecutor::CoStart 启动）。
    CCoroutine()
        : m_pState(std::make_shared<detail::CTaskState<TValue> >()),
          m_pExecutor(),
          m_pValue(),
          m_nStep(0),
          m_bTerminated(false),
          m_reason(detail::kEndNone)
    {
    }

    /// @brief 析构（不阻塞；若 Resume 仍入队，调用方须保证对象存活）。
    virtual ~CCoroutine() {}

    CCoroutine(const CCoroutine&) = delete;
    CCoroutine& operator=(const CCoroutine&) = delete;

    /// @brief 阻塞获取协程最终结果（与 CTask::Get 语义一致，不抛异常）。
    ///
    /// @return 最终结果（有值 / 无值终止，Reason() 区分原因）。
    auto Get() const -> CTaskResult<TValue> { return m_pState->Wait(); }

    /// @brief 协程体（派生类实现，用 CO_BEGIN / ... / CO_END 宏）。
    virtual void Run() = 0;

    /// 允许执行器创建 / 复位 / 调度协程。
    friend class CAsyncExecutor;

protected:
    // ---------------- 宏接口 ----------------

    /// @brief 当前恢复点（状态机步号；CO_BEGIN 的 switch 用）。
    int Step() const { return m_nStep.load(); }

    /// @brief await 任务（非 void）：挂起；值到后恢复，GetValue<U>() 取值。
    ///
    /// @tparam TSubmit 提交函数类型（无参，返回 CTask<U>）。
    /// @param nLine 恢复点标签（宏自动传 __LINE__）。
    /// @param fnSubmit 提交函数（立即调用，取得 CTask 并注册续接）。
    template <typename TSubmit>
    void Await(int nLine, TSubmit fnSubmit)
    {
        using TTask = typename detail::TInvokeResult<TSubmit>::type; // CTask<U>
        using U = typename detail::CCoroutineValue<TTask>::type;     // U（非 void）
        static_assert(!std::is_same<U, void>::value, "void 任务请用 CO_AWAIT_VOID");

        // 先设恢复点，再注册续接（续接异步投递 Resume，此时已可读到 nLine）。
        m_nStep.store(nLine, std::memory_order_release);

        CTask<U> task = fnSubmit();
        detail::CValueHolderImpl<U>* pValue = new detail::CValueHolderImpl<U>();
        m_pValue.reset(pValue);

        bool bOk = task.OnSuccess([this, pValue](const U& v)
        {
            pValue->bHasValue = true;
            pValue->value = v;
            PostResume();
        });
        task.OnNone([this, pValue](detail::CTaskEndReason reason)
        {
            pValue->bHasValue = false;
            pValue->reason = reason;
            MarkTerminated(reason); // await 到无值 → 协程终止（原因透传）。
            PostResume();
        });
        if (!bOk)
        {
            Terminate(detail::kStopped); // 任务已就绪但执行器不可用。
        }
    }

    /// @brief await 任务（void）：挂起；完成后恢复。
    ///
    /// @tparam TSubmit 提交函数类型（无参，返回 CTask<void>）。
    /// @param nLine 恢复点标签（宏自动传 __LINE__）。
    /// @param fnSubmit 提交函数（立即调用，取得 CTask 并注册续接）。
    template <typename TSubmit>
    void AwaitVoid(int nLine, TSubmit fnSubmit)
    {
        using TTask = typename detail::TInvokeResult<TSubmit>::type; // CTask<void>
        using U = typename detail::CCoroutineValue<TTask>::type;     // void
        static_assert(std::is_same<U, void>::value, "非 void 任务请用 CO_AWAIT");

        m_nStep.store(nLine, std::memory_order_release);

        CTask<void> task = fnSubmit();
        detail::CValueHolderImpl<void>* pValue = new detail::CValueHolderImpl<void>();
        m_pValue.reset(pValue);

        bool bOk = task.OnSuccess([this, pValue]()
        {
            pValue->bHasValue = true;
            PostResume();
        });
        task.OnNone([this, pValue](detail::CTaskEndReason reason)
        {
            pValue->bHasValue = false;
            pValue->reason = reason;
            MarkTerminated(reason); // await 到无值 → 协程终止（原因透传）。
            PostResume();
        });
        if (!bOk)
        {
            Terminate(detail::kStopped);
        }
    }

    /// @brief 取最近一次 await 的值（CO_AWAIT 恢复后调用；类型须与任务一致）。
    ///
    /// @tparam U 值类型（须与 await 的任务结果类型一致）。
    /// @return await 到的值。
    template <typename U>
    U GetValue() const
    {
        const detail::CValueHolderImpl<U>* p =
            static_cast<const detail::CValueHolderImpl<U>*>(m_pValue.get());
        return p->value;
    }

    /// @brief 最近一次 await 是否无值终止（None / 异常）。
    bool IsTerminated() const { return m_bTerminated.load(); }

    /// @brief 终止原因（IsTerminated() 为 true 时有效）。
    detail::CTaskEndReason Reason() const
    {
        return static_cast<detail::CTaskEndReason>(m_reason.load());
    }

    /// @brief 协程正常结束（有值）。CO_RETURN 用（void 协程不可用）。
    template <typename V = TValue,
              typename std::enable_if<!std::is_void<V>::value, int>::type = 0>
    void CompleteResult(const V& v)
    {
        m_pState->Complete(CTaskResult<TValue>(v));
    }

    /// @brief 协程正常结束（void 完成）。CO_RETURN_VOID 用。
    void CompleteDone() { m_pState->Complete(CTaskResult<TValue>()); }

    /// @brief 协程终止（无值）。CO_END 兜底 / 框架内部用。
    void CompleteNone(detail::CTaskEndReason reason = detail::kEndNone)
    {
        m_pState->Complete(CTaskResult<TValue>::MakeNone(reason));
    }

private:
    /// @brief 绑定执行器句柄（CoStart 调用；friend CAsyncExecutor）。
    void BindExecutor(const std::shared_ptr<detail::CExecutorHandle>& pExecutor)
    {
        m_pExecutor = pExecutor;
    }

    /// @brief 复位状态（CoStart 调用；同一协程对象可重新 CoStart）。
    void Reset()
    {
        m_pState.reset(new detail::CTaskState<TValue>());
        m_pValue.reset();
        m_nStep.store(0, std::memory_order_relaxed);
        m_bTerminated.store(false, std::memory_order_relaxed);
        m_reason.store(detail::kEndNone, std::memory_order_relaxed);
    }

    /// @brief 把 Resume 投递到执行器（串行调度；执行器不可用 → kStopped 终止）。
    void PostResume()
    {
        if (m_pExecutor != nullptr && m_pExecutor->m_pPool != nullptr &&
            !m_pExecutor->m_bStopped &&
            m_pExecutor->m_pPool->Submit([this]() { Resume(); }))
        {
            return;
        }
        Terminate(detail::kStopped); // 执行器已停止/不可用 → 安全终止，不悬垂。
    }

    /// @brief 在当前线程继续执行协程体（状态机从 m_nStep 恢复）。
    ///
    /// 终止判定由协程体宏完成（case 处 IsTerminated() → CompleteNone），
    /// 此处不拦截，保证终止协程也能走到 Complete（Get() 不阻塞）。
    void Resume()
    {
        Run();
    }

    /// @brief 标记终止（OnNone 回调用；终止原因由协程体宏透传）。
    void MarkTerminated(detail::CTaskEndReason reason)
    {
        m_bTerminated.store(true, std::memory_order_relaxed);
        m_reason.store(reason, std::memory_order_relaxed);
    }

    /// @brief 标记终止并完成（无值）。
    void Terminate(detail::CTaskEndReason reason)
    {
        MarkTerminated(reason);
        CompleteNone(reason);
    }

private:
    std::shared_ptr<detail::CTaskState<TValue> > m_pState; // 协程最终结果。
    std::shared_ptr<detail::CExecutorHandle> m_pExecutor;  // 执行器句柄（Resume 调度）。
    std::unique_ptr<detail::CValueHolder> m_pValue;        // 最近一次 await 的值。
    std::atomic<int> m_nStep;                              // 状态机步号（恢复点）。
    std::atomic<bool> m_bTerminated;                       // await 到无值 → 终止。
    std::atomic<int> m_reason;                             // 终止原因。
};

/// @brief 创建并启动协程（投递首次 Resume；返回 shared_ptr 管理生命周期）。
///
/// @tparam TCoroutine 协程类型（继承 CCoroutine<TValue> 并实现 Run()）。
/// @tparam TArgs 协程构造参数类型。
/// @param args 转发给 TCoroutine 构造函数的参数。
/// @return 协程对象；调用方须持有直到完成（Get() 取结果），勿丢弃。
template <typename TCoroutine, typename... TArgs>
std::shared_ptr<TCoroutine> CAsyncExecutor::CoStart(TArgs&&... args)
{
    std::shared_ptr<TCoroutine> pCoro =
        std::make_shared<TCoroutine>(std::forward<TArgs>(args)...);
    pCoro->BindExecutor(m_pHandle);
    pCoro->Reset();
    pCoro->PostResume(); // 投递首次执行（未启动 → 立即 kStopped 终止）。
    return pCoro;
}

} // namespace async
} // namespace common

// ====================================================================
// 协程体宏（Duff's device 状态机；每个宏独占一行，__LINE__ 作恢复点）。
// 使用形态（派生类成员函数 Run() 内）：
//
//   void Run() override
//   {
//       CO_BEGIN();
//       CO_AWAIT(m_pExec->Submit(读配置));   // 挂起；值到后恢复
//       m_nPort = GetValue<int>();           // 取回值（成员变量）
//       CO_AWAIT_VOID(m_pExec->Post 类任务); // void 任务
//       CO_RETURN(m_nPort * 2);              // 正常结束（有值）
//       CO_END();                            // 兜底：无值终止
//   }
// ====================================================================
#define CO_BEGIN()  switch (Step()) { case 0:;

#define CO_AWAIT(expr) \
    Await(__LINE__, [&]() { return (expr); }); \
    return; \
    case __LINE__: \
    if (IsTerminated()) \
    { \
        CompleteNone(Reason()); \
        return; \
    }

#define CO_AWAIT_VOID(expr) \
    AwaitVoid(__LINE__, [&]() { return (expr); }); \
    return; \
    case __LINE__: \
    if (IsTerminated()) \
    { \
        CompleteNone(Reason()); \
        return; \
    }

#define CO_RETURN(expr) \
    CompleteResult((expr)); \
    return;

#define CO_RETURN_VOID() \
    CompleteDone(); \
    return;

#define CO_END() \
    } \
    CompleteNone(); \
    return;
