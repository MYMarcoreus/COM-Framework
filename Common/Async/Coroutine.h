#pragma once

#include <atomic>
#include <memory>
#include <utility>

#include "Async/AsyncChain.h"
#include "Async/AsyncExecutor.h"
#include "Async/AsyncTypes.h"
#include "Async/SourceLoc.h"
#include "Async/StepResult.h"

// ====================================================================
// 无栈协程（CCoroutine）—— 基于异步链的顺序化写法
//
// 定位：链负责「编排」（层与层串起来，失败即停），协程负责「顺序化」——
// 用顺序代码 await 多条链，替代嵌套回调。两者共用同一套模型：
//   - 协程持有一个共享上下文（std::shared_ptr<TContext>，与链同一实例）；
//   - await 的对象是「链」（含子协程 AsChain() 暴露的链）；
//   - await 结束只告知成功 / 失败，数据一律走共享上下文；
//   - 被等待的链失败 → 协程以该失败码终止（透传，与链的失败即停一致）。
//
// C# 对照：
//   await task;                →  CO_AWAIT(task);            // 等待一条链
//   await Task.WhenAll(a, b);  →  CO_AWAIT_ALL(a, b);        // 并行等待多条链
//   return;                    →  CO_RETURN_VOID(); / CO_END();
//   return result;             →  CO_RETURN(stepResult);
//
// 用法：
// @code
// CStepResult StepLoad(CStepResult upStep, const std::shared_ptr<CMyContext>& spCtx);
// CStepResult StepSave(CStepResult upStep, const std::shared_ptr<CMyContext>& spCtx);
//
// class CMyCoro : public common::async::CCoroutine<CMyContext>
// {
// public:
//     using common::async::CCoroutine<CMyContext>::CCoroutine;   // 继承上下文构造
//
//     void Run() override
//     {
//         CO_BEGIN();
//         CO_AWAIT(Chain(StepLoad));        // 起一条子链并等待（失败则终止）
//         CO_AWAIT_ALL(Chain(StepSave), Chain(StepNotify));   // 并行等待
//         CO_RETURN_VOID();                 // 正常结束
//         CO_END();
//     }
// };
//
// std::shared_ptr<CMyCoro> pCoro = exec.CoStart<CMyCoro>(spCtx);
// common::async::CStepResult r = pCoro->Get();   // 阻塞取最终结果
// (void)r;
// @endcode
//
// 约束（无栈协程固有）：
//  - 跨 await 的变量必须存放为派生类成员（帧），不能用函数内局部变量
//    （局部变量的声明会与 switch-case 恢复点冲突）；
//  - 协程体仍需 CO_BEGIN / CO_END 包裹（Duff's device 的 switch 骨架）；
//  - 每个协程宏独占一行（__LINE__ 作恢复点标签，同一行两个宏会冲突）；
//  - CoStart 返回的 shared_ptr 须持有到完成；框架内部 Resume / 回调捕获自持
//    强引用，提前释放也不会悬垂（对象存活到最后一个 Resume 执行完）。
// ====================================================================

namespace common {
namespace async {

namespace detail {

/// @brief 并行 await 组状态（CO_AWAIT_ALL 用）。
struct CAwaitAllGroup
{
    std::atomic<int> nPending;  ///< 剩余未完成的链数。
    std::atomic<int> bFailed;   ///< 是否有链失败（0/1）。
    std::atomic<int> nCode;     ///< 首个失败码（bFailed 为 1 时有效）。

    CAwaitAllGroup() : nPending(0), bFailed(0), nCode(kStepFailed) {}
};

}  // namespace detail

/// @brief 无栈协程基类（异步链特化版）。
///
/// 派生类实现协程体 Run()，用 CO_BEGIN / CO_AWAIT / CO_RETURN_VOID / CO_END
/// 宏写成「C# async/await」风格：await 即挂起（return 让出线程），被等待的链
/// 完成后由执行器投递 Resume 继续。
///
/// 数据不走 await 返回值，而是共享上下文：协程与它起的子链共用同一个
/// std::shared_ptr<TContext>（GetContext() 取用）。
///
/// @tparam TContext 共享上下文类型。
template <typename TContext>
class CCoroutine
{
   public:
    /// 层函数类型（与链一致：固定签名）。
    using StepFn = detail::StepFn<TContext>;
    /// 完成回调类型。
    using CompletedFn = CCompletedFn;

    /// @brief 创建协程（未绑定执行器；经 CAsyncExecutor::CoStart 启动）。
    ///
    /// @param spContext 共享上下文（可为空：首次 GetContext() 时懒创建）。
    explicit CCoroutine(const std::shared_ptr<TContext>& spContext = std::shared_ptr<TContext>())
        : m_pCore(
              std::make_shared<detail::CChainCore<TContext> >(std::shared_ptr<detail::CExecutorHandle>(), spContext)),
          m_pSegment(std::make_shared<detail::CChainSegment>()),
          m_pExec(nullptr),
          m_wpSelf(),
          m_hot()
    {}

    /// @brief 析构（不阻塞）。
    virtual ~CCoroutine() {}

    /// @brief 不可拷贝（协程帧与运行状态唯一）。
    CCoroutine(const CCoroutine&) = delete;
    CCoroutine& operator=(const CCoroutine&) = delete;

    /// @brief 协程体（派生类实现，用 CO_BEGIN / ... / CO_END 宏）。
    virtual void Run() = 0;

    /// @brief 阻塞获取协程最终结果（不抛异常）。
    ///
    /// @return 最终结果：正常结束为成功；await 到失败 / 执行器停止为对应失败码。
    CStepResult Get() const { return m_pSegment->Wait(); }

    /// @brief 共享上下文（懒创建，恒非空）。
    ///
    /// 协程与它起的子链（Chain()）共用同一实例（TContext 须可默认构造）。
    std::shared_ptr<TContext> GetContext() const { return m_pCore->Context(); }

    /// @brief 本协程作为可等待的链（供外层 CO_AWAIT 或 OnCompleted 使用）。
    ///
    /// 须在 CAsyncExecutor::CoStart 启动之后调用（启动时绑定执行器并复位状态）。
    ///
    /// @return 指向本协程完成状态的链句柄。
    CAsyncChain<TContext> AsChain() const { return CAsyncChain<TContext>::Make(m_pCore, m_pSegment); }

    /// @brief 起一条子链（复用本协程的执行器与共享上下文）。
    ///
    /// 供协程体内 await 使用：CO_AWAIT(Chain(StepLoad))。
    ///
    /// @param fnStep 首层函数（固定签名）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 已投递首层的子链句柄（须先挂起 await，勿丢弃）。
    CAsyncChain<TContext> Chain(const StepFn& fnStep, const CSourceLoc& loc = CSourceLoc()) const
    {
        CAsyncChain<TContext> chain = CAsyncChain<TContext>::Make(m_pCore, std::shared_ptr<detail::CChainSegment>());
        chain.Submit(fnStep, loc);
        return chain;
    }

    /// @brief 在指定执行器上启动协程（绑定 + 复位 + 投递首次执行）。
    ///
    /// 由 CAsyncExecutor::CoStart 调用；执行器须存活于协程生命周期
    /// （未启动 / 已停止时协程立即以失败 kStepStopped 结束）。
    ///
    /// @param pExec 执行器指针。
    void Start(CAsyncExecutor* pExec)
    {
        BindExecutor(pExec);
        Reset();
        PostResume();
    }

    /// @brief 注入自持弱引用（CoStart 调用；Resume / 回调生命周期加固）。
    ///
    /// 使已投递的 Resume 与 await 回调捕获强引用：调用方提前释放 shared_ptr
    /// 后，协程对象仍存活到最后一个 Resume 执行完毕（不悬垂）。
    ///
    /// @param sp 协程对象的 shared_ptr（CoStart 返回的那个）。
    void SetSelf(const std::shared_ptr<void>& sp) { m_wpSelf = sp; }

   protected:
    // ---------------- 宏接口 ----------------

    /// @brief 当前恢复点（状态机步号；CO_BEGIN 的 switch 用）。
    int Step() const { return m_hot.nStep.load(); }

    /// @brief await 一条链（CO_AWAIT 用）：挂起，链结束后恢复。
    ///
    /// 被等待的链失败 → 本协程以该失败码终止（与链的失败即停一致）。
    /// 数据不经返回值传递：协程与被等待的链共用共享上下文。
    ///
    /// @param nLine 恢复点标签（宏自动传 __LINE__）。
    /// @param chain 被等待的链（含子协程 AsChain()）。
    void AwaitWait(int nLine, const CAsyncChain<TContext>& chain)
    {
        m_hot.nStep.store(nLine, std::memory_order_release);

        // 回调捕获自持强引用：保证协程对象存活到回调执行完毕。
        std::shared_ptr<void> spSelf = m_wpSelf.lock();
        const bool bOk = chain.OnCompleted([spSelf, this](CStepResult finalStep)
        {
            if (finalStep.IsFailed())
            {
                MarkTerminated(finalStep);  // 被等待的链失败 → 协程终止（码透传）。
            }
            ResumeInline();  // 负载感知内联 / 投递。
        });
        if (!bOk)
        {
            Terminate(CStepResult::Failed(kStepStopped));  // 无法注册（无效链 / 执行器不可用）。
        }
    }

    /// @brief 并行 await 多条链（CO_AWAIT_ALL 用）：全部完成后恢复。
    ///
    /// 任一条链失败 → 协程以首个失败码终止（仍等所有链结束，避免对象提前释放）。
    ///
    /// @param nLine 恢复点标签（宏自动传 __LINE__）。
    /// @param args 被等待的链列表（可为 Chain(...) 表达式或 AsChain() 句柄）。
    template <typename... TArgs>
    void AwaitAll(int nLine, TArgs&&... args)
    {
        m_hot.nStep.store(nLine, std::memory_order_release);

        std::shared_ptr<detail::CAwaitAllGroup> pGroup = std::make_shared<detail::CAwaitAllGroup>();
        pGroup->nPending.store(static_cast<int>(sizeof...(TArgs)), std::memory_order_relaxed);
        if (sizeof...(TArgs) == 0)
        {
            return;  // 空列表：无需等待（调用方紧接着 return 让出线程即可）。
        }

        AwaitEach(pGroup, std::forward<TArgs>(args)...);
    }

    /// @brief 本协程是否已终止（await 到失败）。
    bool IsTerminated() const { return m_hot.bTerminated.load(); }

    /// @brief 终止失败码（IsTerminated() 为 true 时有效）。
    int TerminateCode() const { return m_hot.nCode.load(); }

    /// @brief 协程以指定结果结束（CO_RETURN 用）。
    ///
    /// @param stepResult 最终结果（成功 / 失败）。
    void CompleteResult(const CStepResult& stepResult) { m_pSegment->Complete(stepResult); }

    /// @brief 协程正常结束（成功）：CO_RETURN_VOID / CO_END 用。
    void CompleteDone() { m_pSegment->Complete(CStepResult::Ok()); }

    /// @brief 协程以终止失败码结束（await 失败后的统一出口）。
    void CompleteTerminated() { m_pSegment->Complete(CStepResult::Failed(TerminateCode())); }

   private:
    /// @brief 协程热状态：步号 / 终止标志 / 终止码（紧邻打包，减少跨线程迁移的 cache line 数）。
    struct CHotState
    {
        std::atomic<int> nStep;         ///< 状态机步号（恢复点）。
        std::atomic<bool> bTerminated;  ///< await 到失败 → 终止。
        std::atomic<int> nCode;         ///< 终止失败码。

        CHotState() : nStep(0), bTerminated(false), nCode(kStepFailed) {}
    };

    /// @brief 绑定执行器（Start 调用）。
    ///
    /// @param pExec 执行器指针（Resume 调度与子链投递用；须存活于协程）。
    void BindExecutor(CAsyncExecutor* pExec)
    {
        m_pExec = pExec;
        if (pExec != nullptr)
        {
            m_pCore->SetHandle(pExec->Handle());  // 子链与本协程共用句柄。
        }
    }

    /// @brief 复位状态（Start 调用；同一协程对象可重新 CoStart）。
    void Reset()
    {
        m_pSegment = std::make_shared<detail::CChainSegment>();
        m_hot.nStep.store(0, std::memory_order_relaxed);
        m_hot.bTerminated.store(false, std::memory_order_relaxed);
        m_hot.nCode.store(kStepFailed, std::memory_order_relaxed);
    }

    /// @brief 把 Resume 投递到执行器（执行器不可用 → 以失败结束，不悬垂）。
    ///
    /// 投递的 Resume 捕获自持强引用：调用方提前释放 shared_ptr 后，协程对象
    /// 仍存活到 Resume 执行完毕。
    void PostResume()
    {
        if (m_pExec == nullptr)
        {
            Terminate(CStepResult::Failed(kStepStopped));
            return;
        }
        std::shared_ptr<void> spSelf = m_wpSelf.lock();
        if (!spSelf)
        {
            Terminate(CStepResult::Failed(kStepStopped));  // 无强引用（理论不应发生）。
            return;
        }
        if (!m_pExec->Post([spSelf, this]() { Resume(); }))
        {
            Terminate(CStepResult::Failed(kStepStopped));  // 执行器已停止 / 不可用。
        }
    }

    /// @brief 内联续接（负载感知）：await 回调已运行在工作线程上，线程池无积压
    ///        时直接在该线程继续执行协程体，省去一次入队 + 唤醒；有积压则投递，
    ///        保持任务级并行度。
    ///
    /// 与链的级联共用线程局部深度计数，限制连续内联层数防爆栈。
    void ResumeInline()
    {
        if (m_pExec == nullptr || m_pExec->IsStopped())
        {
            Terminate(CStepResult::Failed(kStepStopped));
            return;
        }
        if (m_pExec->IsIdle() && detail::InlineDepth() < detail::kMaxInlineDepth)
        {
            ++detail::InlineDepth();
            Resume();
            --detail::InlineDepth();
            return;
        }

        PostResume();  // 有积压 / 深度超限：投递，保并行度 / 防爆栈。
    }

    /// @brief 在当前线程继续执行协程体（状态机从恢复点继续）。
    ///
    /// 终止判定由协程体宏完成（case 处 IsTerminated() → CompleteTerminated()），
    /// 此处不拦截，保证被终止的协程也能走到完成（Get() 不阻塞）。
    void Resume() { Run(); }

    /// @brief 标记终止（不完成；等待协程体走到统一出口）。
    void MarkTerminated(const CStepResult& stepResult)
    {
        m_hot.bTerminated.store(true, std::memory_order_relaxed);
        m_hot.nCode.store(stepResult.Code(), std::memory_order_relaxed);
    }

    /// @brief 标记终止并立即完成（同步失败的出口：协程体不会再被恢复）。
    void Terminate(const CStepResult& stepResult)
    {
        MarkTerminated(stepResult);
        m_pSegment->Complete(stepResult);
    }

    /// @brief 并行 await：递归展开等待列表。
    void AwaitEach(const std::shared_ptr<detail::CAwaitAllGroup>& /*pGroup*/) {}

    /// @brief 并行 await：递归展开等待列表（注册一条链的完成回调）。
    template <typename... TRest>
    void AwaitEach(const std::shared_ptr<detail::CAwaitAllGroup>& pGroup, const CAsyncChain<TContext>& chain,
                   TRest&&... rest)
    {
        std::shared_ptr<void> spSelf = m_wpSelf.lock();
        const bool bOk =
            chain.OnCompleted([pGroup, spSelf, this](CStepResult finalStep) { OnAwaitDone(pGroup, finalStep); });
        if (!bOk)
        {
            OnAwaitDone(pGroup, CStepResult::Failed(kStepStopped));  // 无法注册 → 该链计为失败。
        }
        AwaitEach(pGroup, std::forward<TRest>(rest)...);
    }

    /// @brief 并行 await：一条链结束（记失败码；全部结束时恢复 / 终止）。
    void OnAwaitDone(const std::shared_ptr<detail::CAwaitAllGroup>& pGroup, const CStepResult& stepResult)
    {
        if (stepResult.IsFailed())
        {
            int nExpected = 0;
            if (pGroup->bFailed.compare_exchange_strong(nExpected, 1))
            {
                pGroup->nCode.store(stepResult.Code(), std::memory_order_relaxed);  // 首个失败码。
            }
        }
        if (pGroup->nPending.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            if (pGroup->bFailed.load(std::memory_order_relaxed))
            {
                MarkTerminated(CStepResult::Failed(pGroup->nCode.load(std::memory_order_relaxed)));
            }
            ResumeInline();  // 负载感知内联 / 投递。
        }
    }

    std::shared_ptr<detail::CChainCore<TContext> > m_pCore;  ///< 共享核心（上下文 + 执行器句柄）。
    std::shared_ptr<detail::CChainSegment> m_pSegment;       ///< 协程完成状态（AsChain 暴露）。
    CAsyncExecutor* m_pExec;                                 ///< 执行器指针（Resume 调度 + 子链投递）。
    std::weak_ptr<void> m_wpSelf;                            ///< 自持弱引用（生命周期加固）。
    CHotState m_hot;                                         ///< 热状态（步号 / 终止标志 / 终止码）。
};

/// @brief 起协程实现（执行器入口）：创建 + 注入自持引用 + 启动。
///
/// @tparam TCoroutine 协程类型（继承 CCoroutine<TContext> 并实现 Run()）。
/// @tparam TArgs 协程构造参数类型。
/// @param args 转发给 TCoroutine 构造函数的参数。
/// @return 协程对象；调用方须持有直到完成（Get() 取结果），勿丢弃。
template <typename TCoroutine, typename... TArgs>
std::shared_ptr<TCoroutine> CAsyncExecutor::CoStart(TArgs&&... args)
{
    std::shared_ptr<TCoroutine> pCoro = std::make_shared<TCoroutine>(std::forward<TArgs>(args)...);
    pCoro->SetSelf(pCoro);  // 自持弱引用：Resume / 回调生命周期加固。
    pCoro->Start(this);     // 绑定 + 复位 + 投递首次执行（未启动 / 已停止 → 立即失败结束）。
    return pCoro;
}

}  // namespace async
}  // namespace common

// ====================================================================
// 协程体宏（Duff's device 状态机；每个宏独占一行，__LINE__ 作恢复点）。
// 使用形态（派生类成员函数 Run() 内，C# async/await 风格）：
//
//   void Run() override
//   {
//       CO_BEGIN();
//       CO_AWAIT(Chain(StepLoad));                  // 等待子链（失败则协程终止）
//       CO_AWAIT(pChild->AsChain());                // 等待子协程
//       CO_AWAIT_ALL(Chain(StepA), Chain(StepB));   // 并行等待多条链
//       CO_RETURN_VOID();                           // 正常结束（成功）
//       CO_END();                                   // 兜底：正常结束
//   }
//
// 说明：await 不传递数据（层与层、协程与链之间只传成功 / 失败），数据读写
//       一律通过 GetContext() 得到的共享上下文。
// ====================================================================
#define CO_BEGIN()  \
    switch (Step()) \
    {               \
        case 0:;

#define CO_AWAIT(expr)            \
    AwaitWait(__LINE__, (expr));  \
    return;                       \
    case __LINE__:                \
        if (IsTerminated())       \
        {                         \
            CompleteTerminated(); \
            return;               \
        }

#define CO_AWAIT_ALL(...)            \
    AwaitAll(__LINE__, __VA_ARGS__); \
    return;                          \
    case __LINE__:                   \
        if (IsTerminated())          \
        {                            \
            CompleteTerminated();    \
            return;                  \
        }

#define CO_RETURN(stepResult)     \
    CompleteResult((stepResult)); \
    return;

#define CO_RETURN_VOID() \
    CompleteDone();      \
    return;

#define CO_END()    \
    }               \
    CompleteDone(); \
    return;
