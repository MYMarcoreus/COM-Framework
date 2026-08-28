#pragma once

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>

#include "Async/AsyncExecutor.h"

// ====================================================================
// 无栈协程（CCoroutine）—— 基于 CAsyncExecutor + CTask 的
// Duff's device 状态机，用法贴近 C# async/await。
//
// C# 对照：
//   int a = await task;              →  CO_AWAIT(m_a, task);   // m_a 为帧成员
//   await voidTask;                  →  CO_AWAIT_VOID(voidTask);
//   return value;                    →  CO_RETURN(value);
//
// 核心语义（与 CTask 的 Option 风格一致）：
//  - CO_AWAIT 挂起（return 让出线程，线程回线程池），任务有值/无值后由
//    执行器投递 Resume 继续，结果自动写入目标变量；
//  - await 到无值（None / 异常）→ 协程终止（原因透传，与 CTask 链一致）；
//  - 生命周期：绑定执行器句柄（CExecutorHandle），执行器析构/Stop 后
//    已挂起协程安全以 kStopped 终止，不悬垂。
//
// 约束（无栈协程固有）：
//  - 跨 await 的变量必须存放为派生类成员（帧），不能用函数内局部变量
//    （局部变量声明会与 switch-case 恢复点冲突）；
//  - 协程体仍需 CO_BEGIN / CO_END 包裹（Duff's device 的 switch 骨架）；
//  - 每个协程宏独占一行（__LINE__ 作恢复点标签，同一行两个宏会冲突）；
//  - CoStart 返回的 shared_ptr 必须被持有直到完成（Resume 访问协程对象）。
// ====================================================================

namespace common {
namespace async {

namespace detail {

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
/// 宏写成「C# async/await」风格：await 即挂起（return 让出线程）、值到后
/// 由执行器投递 Resume 继续，结果自动写入目标变量。
///
/// 对照 C#（无栈协程的固有约束）：
///  - `int a = await task;`            → `CO_AWAIT(m_a, task)`（a 存为成员）；
///  - 协程体仍需 CO_BEGIN / CO_END 包裹（Duff's device 的 switch 骨架）；
///  - 跨 await 的变量必须存放为派生类成员（帧），不能用函数内局部变量。
///
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

    /// @brief await 任务并写入目标（C# 风格：CO_AWAIT(target, expr)）。
    ///
    /// 挂起；任务有值时把结果写入 *pTarget 后恢复（恢复后 target 直接用），
    /// 任务无值时标记终止（原因透传，协程体 case 处 CompleteNone）。
    ///
    /// @tparam TSubmit 提交函数类型（无参，返回 CTask<U>）。
    /// @tparam TDst 目标值类型（须与任务结果 U 兼容）。
    /// @param nLine 恢复点标签（宏自动传 __LINE__）。
    /// @param fnSubmit 提交函数（立即调用，取得 CTask 并注册续接）。
    /// @param pTarget 结果写入目标（帧变量地址，如 &m_nPort）。
    template <typename TSubmit, typename TDst>
    void AwaitInto(int nLine, TSubmit fnSubmit, TDst* pTarget)
    {
        using TTask = typename detail::TInvokeResult<TSubmit>::type; // CTask<U>
        using U = typename detail::CCoroutineValue<TTask>::type;     // U（非 void）
        static_assert(!std::is_same<U, void>::value, "void 任务请用 CO_AWAIT_VOID");

        // 先设恢复点，再注册续接（续接异步投递 Resume，此时已可读到 nLine）。
        m_nStep.store(nLine, std::memory_order_release);

        CTask<U> task = fnSubmit();
        bool bOk = task.OnSuccess([this, pTarget](const U& v)
        {
            *pTarget = v; // 结果直接写入目标（帧变量），恢复后即可用。
            PostResume();
        });
        task.OnNone([this](detail::CTaskEndReason reason)
        {
            MarkTerminated(reason); // await 到无值 → 协程终止（原因透传）。
            PostResume();
        });
        if (!bOk)
        {
            Terminate(detail::kStopped); // 任务已就绪但执行器不可用。
        }
    }

    /// @brief await 任务（void）：挂起；完成后恢复（无目标写入）。
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
        bool bOk = task.OnSuccess([this]()
        {
            PostResume();
        });
        task.OnNone([this](detail::CTaskEndReason reason)
        {
            MarkTerminated(reason); // await 到无值 → 协程终止（原因透传）。
            PostResume();
        });
        if (!bOk)
        {
            Terminate(detail::kStopped);
        }
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
// 使用形态（派生类成员函数 Run() 内，C# async/await 风格）：
//
//   void Run() override
//   {
//       CO_BEGIN();
//       // 等价 C#：m_nPort = await 读配置();    （m_nPort 为帧成员）
//       CO_AWAIT(m_nPort, m_pExec->Submit(读配置));
//       // 等价 C#：await 写日志();               （void 任务）
//       CO_AWAIT_VOID(m_pExec->Submit(写日志));
//       CO_RETURN(m_nPort * 2);               // return 值（正常结束）
//       CO_END();                             // 兜底：无值终止
//   }
// ====================================================================
#define CO_BEGIN()  switch (Step()) { case 0:;

#define CO_AWAIT(target, expr) \
    AwaitInto(__LINE__, [&]() { return (expr); }, &(target)); \
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
