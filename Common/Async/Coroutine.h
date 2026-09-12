#pragma once

#include <atomic>
#include <memory>
#include <utility>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "Async/PromiseTypes.h"
#include "Async/SourceLoc.h"

// ====================================================================
// CCoroutine —— 无栈协程（用顺序代码 await 多条 promise）
//
// 定位：promise 负责「编排」（then / catch / finally 串起来，失败即停），
// 协程负责「顺序化」—— 用顺序代码 await 多条 promise，替代回调嵌套。
//
// 两者共用同一套模型：
//   - 协程持有一个共享上下文（std::shared_ptr<TContext>，与它起的子 promise 同一实例）；
//   - await 的对象是「promise」（含子协程 AsPromise() 暴露的 promise）；
//   - await 只告知兑现 / 拒绝，数据一律走共享上下文；
//   - 被等待的 promise 被拒绝 → 协程以该拒绝码终止（透传，与 then 的失败即停一致）。
//
// JS / C# 对照：
//   await p;                     →  CO_AWAIT(p);              // 等待一条 promise
//   await Promise.all([a, b]);   →  CO_AWAIT_ALL(a, b);       // 并行等待多条 promise
//   return;                      →  CO_RETURN_VOID(); / CO_END();
//   return result;               →  CO_RETURN(CPromiseResult::Reject(码));
//   局部变量跨 await             →  必须写成派生类成员（无栈约束）
//
// 用法：
// @code
// CPromiseResult StepLoad(CPromiseResult upResult, const std::shared_ptr<CMyContext>& spCtx);
// CPromiseResult StepSave(CPromiseResult upResult, const std::shared_ptr<CMyContext>& spCtx);
//
// class CMyCoroutine : public common::async::CCoroutine<CMyContext>
// {
// public:
//     using common::async::CCoroutine<CMyContext>::CCoroutine;   // 继承上下文构造
//
//     void Run() override
//     {
//         CO_BEGIN();
//         CO_AWAIT(NewPromise(StepLoad));     // 起一条子 promise 并等待（被拒绝则终止）
//         CO_AWAIT_ALL(NewPromise(StepSave), NewPromise(StepNotify));   // 并行等待
//         CO_RETURN_VOID();                   // 正常结束（兑现）
//         CO_END();
//     }
// };
//
// std::shared_ptr<CMyCoroutine> pCoro = exec.CoStart<CMyCoroutine>(spCtx);
// common::async::CPromiseResult r = pCoro->Await();   // 阻塞取最终结果
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
    std::atomic<int> nPending;   ///< 剩余未完成的 promise 数。
    std::atomic<int> bRejected;  ///< 是否已有 promise 被拒绝（0/1）。
    std::atomic<int> nCode;      ///< 首个拒绝码（bRejected 为 1 时有效）。

    CAwaitAllGroup() : nPending(0), bRejected(0), nCode(kRejected)
    {}
};

}  // namespace detail

/// @brief 无栈协程基类（基于异步 promise 的顺序化写法）。
///
/// 派生类实现协程体 Run()，用 CO_BEGIN / CO_AWAIT / CO_RETURN_VOID / CO_END
/// 宏写成「async/await」风格：await 即挂起（return 让出线程），被等待的
/// promise settled 后由执行器投递 Resume 继续。
///
/// 数据不走 await 返回值，而是共享上下文：协程与它起的子 promise 共用同一个
/// std::shared_ptr<TContext>（GetContext() 取用）。
///
/// @tparam TContext 共享上下文类型。
template <typename TContext>
class CCoroutine
{
public:
    //================ Types ================

    /// 处理器类型（与 promise 一致：固定签名）。
    using ThenHandler = detail::ThenHandler<TContext>;

    //================ Lifecycle ================

    /// @brief 创建协程（未绑定执行器；经 CAsyncExecutor::CoStart 启动）。
    ///
    /// @param spContext 共享上下文（可为空：首次 GetContext() 时懒创建）。
    explicit CCoroutine(const std::shared_ptr<TContext>& spContext = std::shared_ptr<TContext>())
        : m_pCore(
              std::make_shared<detail::CPromiseCore<TContext> >(std::shared_ptr<detail::CExecutorHandle>(), spContext)),
          m_pSegment(std::make_shared<detail::CPromiseState>()),
          m_pExec(nullptr),
          m_wpSelf(),
          m_hot()
    {}

    /// @brief 析构（不阻塞）。
    virtual ~CCoroutine()
    {}

    /// @brief 不可拷贝（协程帧与运行状态唯一）。
    CCoroutine(const CCoroutine&) = delete;
    CCoroutine& operator=(const CCoroutine&) = delete;

    /// @brief 协程体（派生类实现，用 CO_BEGIN / ... / CO_END 宏）。
    virtual void Run() = 0;

    //================ Context & Await ================

    /// @brief await：阻塞获取协程最终结果（JS await 的阻塞版，不抛异常）。
    ///
    /// @return 最终结果：正常结束为兑现；await 到拒绝 / 执行器停止为对应拒绝码。
    CPromiseResult Await() const
    {
        return m_pSegment->Await();
    }

    /// @brief 共享上下文（懒创建，恒非空）。
    ///
    /// 协程与它起的子 promise（NewPromise()）共用同一实例（TContext 须可默认构造）。
    std::shared_ptr<TContext> GetContext() const
    {
        return m_pCore->Context();
    }

    /// @brief 本协程作为可等待的 promise（供外层 CO_AWAIT 或 OnSettled 使用）。
    ///
    /// 须在 CAsyncExecutor::CoStart 启动之后调用（启动时绑定执行器并复位状态）。
    ///
    /// @return 指向本协程完成状态的 promise 句柄。
    CPromise<TContext> AsPromise() const
    {
        return CPromise<TContext>::Make(m_pCore, m_pSegment);
    }

    /// @brief 起一条子 promise（复用本协程的执行器与共享上下文）。
    ///
    /// 供协程体内 await 使用：CO_AWAIT(NewPromise(StepLoad))。
    ///
    /// @param fnHandler 首层处理器（固定签名）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 已投递首层的 promise 句柄（须先挂起 await，勿丢弃）。
    CPromise<TContext> NewPromise(const ThenHandler& fnHandler, const CSourceLoc& loc = CSourceLoc()) const
    {
        CPromise<TContext> promise = CPromise<TContext>::Make(m_pCore, std::shared_ptr<detail::CPromiseState>());
        promise.Then(fnHandler, loc);  // 首个 Then 即首层（起点结果视为已兑现）。
        return promise;
    }

protected:
    //================ Macro API ================

    /// @brief 当前恢复点（状态机步号；CO_BEGIN 的 switch 用）。
    int Step() const
    {
        return m_hot.nStep.load();
    }

    /// @brief await 一条 promise（CO_AWAIT 用）：挂起，promise settled 后恢复。
    ///
    /// 被等待的 promise 被拒绝 → 本协程以该拒绝码终止（与 then 的失败即停一致）。
    /// 数据不经返回值传递：协程与被等待的 promise 共用共享上下文。
    ///
    /// @param nLine 恢复点标签（宏自动传 __LINE__）。
    /// @tparam TOtherContext 被等待 promise 的上下文类型（**可与本协程不同** —— 支持把
    ///         别的子流程（另一套 TContext）当作一个异步步骤等进来）。
    /// @param promise 被等待的 promise（含子协程 AsPromise()）。
    template <typename TOtherContext>
    void AwaitWait(int nLine, const CPromise<TOtherContext>& promise)
    {
        m_hot.nStep.store(nLine, std::memory_order_release);

        // 回调捕获自持强引用：保证协程对象存活到回调执行完毕。
        std::shared_ptr<void> spSelf = m_wpSelf.lock();
        const bool bOk = promise.OnSettled(
            [spSelf, this](CPromiseResult result)
            {
                if (result.IsRejected())
                {
                    MarkTerminated(result);  // 被等待的 promise 被拒绝 → 协程终止（码透传）。
                }
                ResumeInline();  // 线程亲和 + 负载感知内联 / 投递。
            });
        if (!bOk)
        {
            // 防御：OnSettled 已保证送达（仅无效 promise 返回 false），正常路径不会走到这里。
            Terminate(CPromiseResult::Reject(kStopped));
        }
    }

    /// @brief 并行 await 多条 promise（CO_AWAIT_ALL 用）：全部 settled 后恢复。
    ///
    /// 任一条被拒绝 → 协程以首个拒绝码终止（仍等全部结束，避免对象提前释放）。
    ///
    /// @param nLine 恢复点标签（宏自动传 __LINE__）。
    /// @param args 被等待的 promise 列表（可为 NewPromise(...) 表达式、AsPromise() 句柄，
    ///             或**其它上下文类型**的子流程 promise）。
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

    /// @brief 本协程是否已终止（await 到拒绝）。
    bool IsTerminated() const
    {
        return m_hot.bTerminated.load();
    }

    /// @brief 终止拒绝码（IsTerminated() 为 true 时有效）。
    int TerminateCode() const
    {
        return m_hot.nCode.load();
    }

    /// @brief 协程以指定结果结束（CO_RETURN 用）。
    ///
    /// @param result 最终结果（兑现 / 拒绝）。
    void CompleteResult(const CPromiseResult& result)
    {
        m_pSegment->Settle(result);
    }

    /// @brief 协程正常结束（兑现）：CO_RETURN_VOID / CO_END 用。
    void CompleteDone()
    {
        m_pSegment->Settle(CPromiseResult::Resolve());
    }

    /// @brief 协程以终止拒绝码结束（await 到拒绝后的统一出口）。
    void CompleteTerminated()
    {
        m_pSegment->Settle(CPromiseResult::Reject(TerminateCode()));
    }

private:
    //================ Internal ================

    friend class CAsyncExecutor;  // Start / SetSelf（CoStart 启动路径）。

    /// @brief 在指定执行器上启动协程（绑定 + 复位 + 投递首次执行）。
    ///
    /// 由 CAsyncExecutor::CoStart 调用；执行器须存活于协程生命周期
    /// （未启动 / 已停止时协程立即以 kStopped 被拒绝）。
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
    void SetSelf(const std::shared_ptr<void>& sp)
    {
        m_wpSelf = sp;
    }

    /// @brief 协程热状态：步号 / 终止标志 / 拒绝码（紧邻打包，减少跨线程迁移的 cache line 数）。
    struct CHotState
    {
        std::atomic<int> nStep;         ///< 状态机步号（恢复点）。
        std::atomic<bool> bTerminated;  ///< await 到拒绝 → 终止。
        std::atomic<int> nCode;         ///< 终止拒绝码。

        CHotState() : nStep(0), bTerminated(false), nCode(kRejected)
        {}
    };

    /// @brief 绑定执行器（Start 调用）。
    ///
    /// @param pExec 执行器指针（Resume 调度与子 promise 投递用；须存活于协程）。
    void BindExecutor(CAsyncExecutor* pExec)
    {
        m_pExec = pExec;
        if (pExec != nullptr)
        {
            m_pCore->SetHandle(pExec->Handle());  // 子 promise 与本协程共用句柄。
        }
    }

    /// @brief 复位状态（Start 调用；启动前清空上一次运行的状态）。
    void Reset()
    {
        m_pSegment = std::make_shared<detail::CPromiseState>();
        m_hot.nStep.store(0, std::memory_order_relaxed);
        m_hot.bTerminated.store(false, std::memory_order_relaxed);
        m_hot.nCode.store(kRejected, std::memory_order_relaxed);
    }

    /// @brief 把 Resume 投递到执行器（执行器不可用 → 以拒绝结束，不悬垂）。
    ///
    /// 投递的 Resume 捕获自持强引用：调用方提前释放 shared_ptr 后，协程对象
    /// 仍存活到 Resume 执行完毕。
    void PostResume()
    {
        if (m_pExec == nullptr)
        {
            Terminate(CPromiseResult::Reject(kStopped));
            return;
        }
        std::shared_ptr<void> spSelf = m_wpSelf.lock();
        if (!spSelf)
        {
            Terminate(CPromiseResult::Reject(kStopped));  // 无强引用（理论不应发生）。
            return;
        }
        if (!m_pExec->Post(
                [spSelf, this]()
                {
                    Resume();
                }))
        {
            Terminate(CPromiseResult::Reject(kStopped));  // 执行器已停止 / 不可用。
        }
    }

    /// @brief 内联续接（线程亲和 + 负载感知）：await 回调已运行在工作线程上，
    ///        只有当前线程就是本协程自己的执行器线程、且线程池无积压时才直接继续执行
    ///        协程体（省去一次入队 + 唤醒）；跨执行器（典型：等别的模块的 promise）或有
    ///        积压则投递 —— 保证协程体始终跑在自己的执行器线程上。
    ///
    /// 与 promise 的级联共用线程局部深度计数，限制连续内联层数防爆栈。
    void ResumeInline()
    {
        if (m_pExec == nullptr || m_pExec->IsStopped())
        {
            Terminate(CPromiseResult::Reject(kStopped));
            return;
        }
        // 就地判定与 promise 层派发共用一处（多一条「线程池无积压」的负载感知条件）：
        // 有积压时投递回本执行器，保住并行度。
        if (detail::ShouldInline(detail::kAffinityChain, m_pExec->Handle(), /* bRequireIdle = */ true))
        {
            detail::CInlineGuard guard;  // 深度 +1 / -1 成对。
            Resume();
            return;
        }

        PostResume();  // 跨执行器 / 有积压 / 深度超限：投递，回本执行器线程 / 保并行度 / 防爆栈。
    }

    /// @brief 在当前线程继续执行协程体（状态机从恢复点继续）。
    ///
    /// 终止判定由协程体宏完成（case 处 IsTerminated() → CompleteTerminated()），
    /// 此处不拦截，保证被终止的协程也能走到完成（Await() 不阻塞）。
    void Resume()
    {
        Run();
    }

    /// @brief 标记终止（不 settle；等待协程体走到统一出口）。
    void MarkTerminated(const CPromiseResult& result)
    {
        m_hot.bTerminated.store(true, std::memory_order_relaxed);
        m_hot.nCode.store(result.Code(), std::memory_order_relaxed);
    }

    /// @brief 标记终止并立即 settle（同步失败的出口：协程体不会再被恢复）。
    void Terminate(const CPromiseResult& result)
    {
        MarkTerminated(result);
        m_pSegment->Settle(result);
    }

    /// @brief 并行 await：递归展开等待列表。
    void AwaitEach(const std::shared_ptr<detail::CAwaitAllGroup>& /*pGroup*/)
    {}

    /// @brief 并行 await：递归展开等待列表（注册一条 promise 的 settled 通知）。
    ///
    /// @tparam TOtherContext 被等待 promise 的上下文类型（允许与协程不同）。
    /// @tparam TRest 其余被等待的 promise。
    /// @param pGroup 并行组状态。
    /// @param promise 当前注册的 promise。
    /// @param rest 其余 promise。
    template <typename TOtherContext, typename... TRest>
    void AwaitEach(
        const std::shared_ptr<detail::CAwaitAllGroup>& pGroup, const CPromise<TOtherContext>& promise, TRest&&... rest)
    {
        std::shared_ptr<void> spSelf = m_wpSelf.lock();
        const bool bOk = promise.OnSettled(
            [pGroup, spSelf, this](CPromiseResult result)
            {
                OnAwaitDone(pGroup, result);
            });
        if (!bOk)
        {
            OnAwaitDone(pGroup, CPromiseResult::Reject(kStopped));  // 无法注册 → 该 promise 计为拒绝。
        }
        AwaitEach(pGroup, std::forward<TRest>(rest)...);
    }

    /// @brief 并行 await：一条 promise settled（记首个拒绝码；全部结束时恢复 / 终止）。
    void OnAwaitDone(const std::shared_ptr<detail::CAwaitAllGroup>& pGroup, const CPromiseResult& result)
    {
        if (result.IsRejected())
        {
            int nExpected = 0;
            if (pGroup->bRejected.compare_exchange_strong(nExpected, 1))
            {
                pGroup->nCode.store(result.Code(), std::memory_order_relaxed);  // 首个拒绝码。
            }
        }
        if (pGroup->nPending.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            if (pGroup->bRejected.load(std::memory_order_relaxed))
            {
                MarkTerminated(CPromiseResult::Reject(pGroup->nCode.load(std::memory_order_relaxed)));
            }
            ResumeInline();  // 线程亲和 + 负载感知：就地续跑或投递回本执行器。
        }
    }

    std::shared_ptr<detail::CPromiseCore<TContext> > m_pCore;  ///< 共享核心（上下文 + 执行器句柄）。
    std::shared_ptr<detail::CPromiseState> m_pSegment;         ///< 协程完成状态（AsPromise 暴露）。
    CAsyncExecutor* m_pExec;       ///< 执行器指针（Resume 调度 + 子 promise 投递）。
    std::weak_ptr<void> m_wpSelf;  ///< 自持弱引用（生命周期加固）。
    CHotState m_hot;               ///< 热状态（步号 / 终止标志 / 拒绝码）。
};

/// @brief 起协程实现（执行器入口）：创建 + 注入自持引用 + 启动。
///
/// @tparam TCoroutine 协程类型（继承 CCoroutine<TContext> 并实现 Run()）。
/// @tparam TArgs 协程构造参数类型。
/// @param args 转发给 TCoroutine 构造函数的参数。
/// @return 协程对象；调用方须持有直到完成（Await() 取结果），勿丢弃。
template <typename TCoroutine, typename... TArgs>
std::shared_ptr<TCoroutine> CAsyncExecutor::CoStart(TArgs&&... args)
{
    std::shared_ptr<TCoroutine> pCoro = std::make_shared<TCoroutine>(std::forward<TArgs>(args)...);
    pCoro->SetSelf(pCoro);  // 自持弱引用：Resume / 回调生命周期加固。
    pCoro->Start(this);     // 绑定 + 复位 + 投递首次执行（未启动 / 已停止 → 立即被拒绝）。
    return pCoro;
}

}  // namespace async
}  // namespace common

// ====================================================================
// 协程体宏（Duff's device 状态机；每个宏独占一行，__LINE__ 作恢复点）。
// 使用形态（派生类成员函数 Run() 内，async/await 风格）：
//
//   void Run() override
//   {
//       CO_BEGIN();
//       CO_AWAIT(NewPromise(StepLoad));                  // 等待子 promise（被拒绝则终止）
//       CO_AWAIT(pChild->AsPromise());                   // 等待子协程
//       CO_AWAIT_ALL(NewPromise(StepA), NewPromise(StepB));  // 并行等待多条 promise
//       CO_RETURN_VOID();                                // 正常结束（兑现）
//       CO_END();                                        // 兜底：正常结束
//   }
//
// 说明：await 不传递数据（层与层、协程与 promise 之间只传兑现 / 拒绝），
//       数据读写一律通过 GetContext() 得到的共享上下文。
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

#define CO_RETURN(result)     \
    CompleteResult((result)); \
    return;

#define CO_RETURN_VOID() \
    CompleteDone();      \
    return;

#define CO_END()    \
    }               \
    CompleteDone(); \
    return;
