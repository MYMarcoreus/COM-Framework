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
//   int a = await task;              →  CO_AWAIT(m_a, task);    // m_a 为帧成员
//   int a = await Foo();             →  CO_AWAIT(m_a, []{ return Foo(); });
//   await voidTask;                  →  CO_AWAIT_VOID(voidTask);
//   return value;                    →  CO_RETURN(value);
//
// CO_AWAIT 的「任务」支持两种传法：
//  - 已提交任务（CTask<U>，可链式）：CO_AWAIT(m_a, exec.Submit(fn).Then(...))
//  - 裸 lambda / 函数对象：自动投递到执行器执行（免写 Submit）
// target 可为任意可写左值（成员、*sp 解引用、sp->field），恢复后自动写入。
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

/// @brief 判断表达式类型是否为 CTask<U>（true：直接用；false：裸 lambda 自动 Submit）。
template <typename T>
struct IsCTask
{
    static const bool value = (TaskTraits<T>::Kind == 1);
};

} // namespace detail

/// @brief 无栈协程基类（Option 风格，基于 CAsyncExecutor + CTask）。
///
/// 派生类实现协程体 Run()，用 CO_BEGIN / CO_AWAIT / CO_RETURN / CO_END
/// 宏写成「C# async/await」风格：await 即挂起（return 让出线程）、值到后
/// 由执行器投递 Resume 继续，结果自动写入目标变量。
///
/// CO_AWAIT 的「任务」可直接传裸 lambda / 函数对象（自动 Submit 到执行器），
/// 或已提交的 CTask（支持链式）；target 可为任意可写左值（成员、*sp 解引用）。
///
/// 数据传递（无栈协程的固有约束，跨 await 变量不能用函数内局部变量）：
///  - 简单值：存为派生类成员（帧）；
///  - 更灵活：用 shared_ptr 成员持有数据对象，CO_AWAIT(*sp, ...) 写入，
///    任务 lambda 可捕获 sp 跨任务传递（shared_ptr 须随协程常驻）。
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
          m_pExec(nullptr),
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
    /// expr 支持两种：
    ///  - 已提交任务 CTask<U>（可链式 Then/flatMap）；
    ///  - 裸 lambda / 函数对象（返回 U）：自动 Submit 到执行器执行。
    /// 挂起；任务有值时把结果写入 *pTarget 后恢复（恢复后 target 直接用），
    /// 任务无值时标记终止（原因透传，协程体 case 处 CompleteNone）。
    ///
    /// @tparam TExpr 任务表达式类型（CTask<U> 或可调用对象）。
    /// @tparam TDst 目标值类型（须与任务结果 U 兼容）。
    /// @param nLine 恢复点标签（宏自动传 __LINE__）。
    /// @param expr 任务（已提交 CTask 或裸 lambda）。
    /// @param pTarget 结果写入目标（任意可写左值地址，如 &m_nPort、&*sp）。
    template <typename TExpr, typename TDst>
    void AwaitInto(int nLine, TExpr expr, TDst* pTarget)
    {
        DoAwait(nLine, expr, pTarget,
                std::integral_constant<bool,
                    detail::IsCTask<typename std::decay<TExpr>::type>::value>());
    }

    /// @brief await void 任务（C# 风格：CO_AWAIT_VOID(expr)）。
    ///
    /// expr 支持已提交 CTask<void> 或裸 lambda（返回 void，自动 Submit）。
    ///
    /// @tparam TExpr 任务表达式类型。
    /// @param nLine 恢复点标签（宏自动传 __LINE__）。
    /// @param expr 任务（已提交 CTask<void> 或裸 lambda）。
    template <typename TExpr>
    void AwaitVoid(int nLine, TExpr expr)
    {
        DoAwaitVoid(nLine, expr,
                    std::integral_constant<bool,
                        detail::IsCTask<typename std::decay<TExpr>::type>::value>());
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
    /// @brief 绑定执行器（CoStart 调用；friend CAsyncExecutor）。
    ///
    /// @param pExecutor 执行器句柄（Resume 调度 / 生命周期）。
    /// @param pExec 执行器指针（自动 Submit 裸 lambda 用；须存活于协程）。
    void BindExecutor(const std::shared_ptr<detail::CExecutorHandle>& pExecutor,
                      CAsyncExecutor* pExec)
    {
        m_pExecutor = pExecutor;
        m_pExec = pExec;
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

    // ---- await 分派与续接注册（AwaitInto / AwaitVoid 内部）----

    /// 已提交任务 CTask<U> → 直接用。
    template <typename TExpr, typename TDst>
    void DoAwait(int nLine, TExpr expr, TDst* pTarget, std::true_type)
    {
        using R = typename std::decay<TExpr>::type;
        using U = typename detail::TaskTraits<R>::ValueType;
        static_assert(!std::is_same<U, void>::value, "void 任务请用 CO_AWAIT_VOID");
        m_nStep.store(nLine, std::memory_order_release);
        CTask<U> task = expr; // expr 为 CTask<U>。
        RegisterAwait(task, pTarget);
    }

    /// 裸 lambda / 函数对象 → 自动 Submit 到执行器。
    template <typename TExpr, typename TDst>
    void DoAwait(int nLine, TExpr expr, TDst* pTarget, std::false_type)
    {
        using R = typename std::decay<TExpr>::type;
        using U = typename detail::TInvokeResult<R>::type;
        static_assert(!std::is_same<U, void>::value, "void 任务请用 CO_AWAIT_VOID");
        m_nStep.store(nLine, std::memory_order_release);
        if (m_pExec == nullptr)
        {
            Terminate(detail::kStopped);
            return;
        }
        CTask<U> task = m_pExec->Submit(expr);
        RegisterAwait(task, pTarget);
    }

    /// 已提交任务 CTask<void> → 直接用。
    template <typename TExpr>
    void DoAwaitVoid(int nLine, TExpr expr, std::true_type)
    {
        using R = typename std::decay<TExpr>::type;
        using U = typename detail::TaskTraits<R>::ValueType;
        static_assert(std::is_same<U, void>::value, "非 void 任务请用 CO_AWAIT");
        m_nStep.store(nLine, std::memory_order_release);
        CTask<void> task = expr; // expr 为 CTask<void>。
        RegisterAwaitVoid(task);
    }

    /// 裸 lambda（返回 void）→ 自动 Submit。
    template <typename TExpr>
    void DoAwaitVoid(int nLine, TExpr expr, std::false_type)
    {
        using R = typename std::decay<TExpr>::type;
        using U = typename detail::TInvokeResult<R>::type;
        static_assert(std::is_same<U, void>::value, "非 void 任务请用 CO_AWAIT");
        m_nStep.store(nLine, std::memory_order_release);
        if (m_pExec == nullptr)
        {
            Terminate(detail::kStopped);
            return;
        }
        CTask<void> task = m_pExec->Submit(expr);
        RegisterAwaitVoid(task);
    }

    /// 注册非 void 任务续接：有值写 pTarget 并恢复；无值标记终止并恢复。
    template <typename U, typename TDst>
    void RegisterAwait(CTask<U>& task, TDst* pTarget)
    {
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

    /// 注册 void 任务续接：完成恢复；无值标记终止并恢复。
    void RegisterAwaitVoid(CTask<void>& task)
    {
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

private:
    std::shared_ptr<detail::CTaskState<TValue> > m_pState; // 协程最终结果。
    std::shared_ptr<detail::CExecutorHandle> m_pExecutor;  // 执行器句柄（Resume 调度）。
    CAsyncExecutor* m_pExec;                               // 执行器指针（自动 Submit 用）。
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
    pCoro->BindExecutor(m_pHandle, this);
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
//       // 等价 C#：m_nPort = await 读配置();   （裸 lambda 自动 Submit）
//       CO_AWAIT(m_nPort, []() { return 读配置(); });
//       // 等价 C#：m_strDb = await 连DB(m_nPort); （捕获 this 读成员）
//       CO_AWAIT(m_strDb, [this]() { return 连DB(m_nPort); });
//       // 等价 C#：await 写日志();              （void 裸 lambda 自动 Submit）
//       CO_AWAIT_VOID([this]() { 写日志(m_strDb); });
//       CO_RETURN(结果(m_nPort, m_strDb));     // return 值（正常结束）
//       CO_END();                             // 兜底：无值终止
//   }
// ====================================================================
#define CO_BEGIN()  switch (Step()) { case 0:;

#define CO_AWAIT(target, expr) \
    AwaitInto(__LINE__, (expr), &(target)); \
    return; \
    case __LINE__: \
    if (IsTerminated()) \
    { \
        CompleteNone(Reason()); \
        return; \
    }

#define CO_AWAIT_VOID(expr) \
    AwaitVoid(__LINE__, (expr)); \
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
