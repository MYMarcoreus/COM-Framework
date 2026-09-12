#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "Async/Diagnostics.h"
#include "Async/PromiseResult.h"
#include "Async/PromiseTypes.h"
#include "Async/SourceLoc.h"
#include "Thread/ThreadPool.h"

// ====================================================================
// 异步执行器（调度层）
//
// 职责：持有工作线程池，提供「投递执行」与「起链入口」—— 起 promise / 起协程
// （`NewPromise` / `CoStart` / 组合器）以及把多个子 promise 汇成一条聚合链的
// 「并行组合」（`WhenAll` 一族）。
// 不做链式编排（`Then` 一族见 Promise.h）、不做顺序化（见 Coroutine/Coroutine.h）。
//
// 用法：
//   common::async::CAsyncExecutor exec(4);
//   exec.Start();
//   exec.Post([]() { /* 无返回值任务 */ });
//   auto p = exec.NewPromise(spCtx, StepLoad).Then(StepSave);   // 起 promise
//   exec.Stop();
//
// 生命周期：执行器析构会停止线程池并等待已投递任务完成；promise / 协程通过
// 共享句柄引用线程池，执行器析构后已起动的 promise 仍安全跑完（新投递以
// kStopped 被拒绝）。
// ====================================================================

namespace common {
namespace async {

template <typename TContext>
class CPromise;  // 前置声明（NewPromise / WhenAll 返回 promise 句柄）。

template <typename TContext>
class CCoroutine;  // 前置声明（CoStart 返回协程句柄）。

namespace detail {

/// @brief 执行器侧诊断文案（集中一处：测试断言常量，而不是去匹配子串）。
constexpr const char* kDiagPostThrow = "exec.Post() 投递的任务抛出了异常（已兜住，未终止进程）";
constexpr const char* kDiagPostEmpty = "exec.Post(): 任务为空（未提交）";

/// @brief 执行器句柄（生命周期加固核心）。
///
/// promise / 协程持有本句柄：执行器析构后线程池对象仍存活（已投递任务跑完），
/// 新投递被 m_bStopped 拒绝并转为拒绝结果。
struct CExecutorHandle
{
    std::shared_ptr<common::thread::CThreadPool> m_pPool;  ///< 工作线程池。
    std::atomic<bool> m_bStopped;                          ///< 是否已停止（拒绝新投递）。

    CExecutorHandle() : m_bStopped(false)
    {}
};

/// @brief 向执行器句柄投递任务（句柄不可用时返回 false，不抛异常）。
///
/// @param pHandle 执行器句柄。
/// @param fnTask 任务函数（移动投递）。
/// @return true 投递成功；false 句柄不可用（空 / 已停止 / 线程池拒绝）。
inline bool PostToHandle(const std::shared_ptr<CExecutorHandle>& pHandle, std::function<void()> fnTask)
{
    if (pHandle == nullptr || pHandle->m_pPool == nullptr || pHandle->m_bStopped)
    {
        return false;
    }
    return pHandle->m_pPool->Submit(std::move(fnTask));
}

/// @brief 当前线程是否是某执行器的工作线程（线程亲和判定）。
///
/// 层处理器只在「已经在本链执行器线程上」时就地内联，否则投递回本链执行器，
/// 从而保证「每一层都跑在它所属链的执行器线程上」。
///
/// @param pHandle 执行器句柄。
/// @return true 当前线程是该执行器的工作线程。
inline bool IsInExecutorThread(const std::shared_ptr<CExecutorHandle>& pHandle)
{
    return pHandle != nullptr && common::thread::CThreadPool::IsInPoolThread(pHandle->m_pPool.get());
}

/// @brief 层处理器的执行线程偏好（线程亲和，默认 `kAffinityChain`）。
///
/// 属于**调度**（跑在哪条线程上），所以归执行器侧；`HandlerMode`（then / catch / finally）
/// 描述的是「层语义」，归 promise 侧。
enum HandlerAffinity
{
    kAffinityChain = 0,    ///< 默认：本链执行器线程（同执行器内联；跨执行器投递回本链执行器）。
    kAffinityInline = 1,   ///< 就地：在「结算本层的那条线程」上执行（不投递，不要求线程亲和）。
    kAffinityExecutor = 2  ///< 指定执行器：在给定执行器线程上执行（同线程内联，否则投递）。
};

/// @brief 级联内联深度（线程局部）：链逐层级联时最多连续内联多少层。
///
/// 超限则改为投递执行，避免超长链（数千层）在递归中爆栈。
const int kMaxInlineDepth = 64;

/// @brief 当前线程的级联内联深度。
inline int& InlineDepth()
{
    static thread_local int s_nInlineDepth = 0;
    return s_nInlineDepth;
}

/// @brief 内联深度的 RAII 守卫（构造 +1，析构 -1）。
///
/// 三处「就地内联」（层处理器 / 通知就地送达 / 协程续跑）都要配对增减；
/// 用守卫可以避免异常或提前 `return` 漏减（漏减会让后续层被多投递，且会累积）。
struct CInlineGuard
{
    CInlineGuard()
    {
        ++InlineDepth();
    }

    ~CInlineGuard()
    {
        --InlineDepth();
    }

    CInlineGuard(const CInlineGuard&) = delete;
    CInlineGuard& operator=(const CInlineGuard&) = delete;
};

/// @brief 解析本层实际使用的执行器句柄（亲和三档）。
///
/// @param eAffinity 亲和三档（`kAffinityChain` / `kAffinityInline` / `kAffinityExecutor`）。
/// @param pTarget 调用方指定的执行器（仅 `kAffinityExecutor` 且非空时生效）。
/// @param pChainHandle 本链执行器句柄（默认值）。
/// @return 本层应使用的执行器句柄。
inline const std::shared_ptr<CExecutorHandle>& ResolveExecHandle(HandlerAffinity eAffinity,
    const std::shared_ptr<CExecutorHandle>& pTarget, const std::shared_ptr<CExecutorHandle>& pChainHandle)
{
    // 注意：pTarget 为空是**合法**的（= 没指定目标 → 退回本链执行器），
    // 所以这里不能断言非空 —— 调用方（如延迟链的 Start）会借此解析「未指定」的形态。
    return (eAffinity == kAffinityExecutor && pTarget != nullptr) ? pTarget : pChainHandle;
}

/// @brief 是否应当**就地内联**（不投递、在当前线程上接着跑）。
///
/// 「就地还是投递」的唯一判定处（promise 的层派发与协程的续跑共用）：
///  - `kAffinityInline`：无条件就地（在结算线程上跑）；
///  - 其余档位：仅当已在目标执行器线程上时就地（省一次入队 + 保序）；
///  - 连续内联超过 `kMaxInlineDepth`：内联（防超长链爆栈）；
///  - `bRequireIdle`：还要求线程池无积压（协程续跑用 —— 有积压时投递，保住并行度）。
///
/// @param eAffinity 亲和三档。
/// @param pExec 目标执行器句柄（调用方已用 `ResolveExecHandle` 解析好）。
/// @param bRequireIdle 是否要求线程池无积压才内联。
/// @return true = 调用方应当直接执行任务体；false = 应当投递。
inline bool ShouldInline(HandlerAffinity eAffinity, const std::shared_ptr<CExecutorHandle>& pExec, bool bRequireIdle = false)
{
    const bool bInline = (eAffinity == kAffinityInline) || IsInExecutorThread(pExec);
    if (!bInline || InlineDepth() >= kMaxInlineDepth)
    {
        return false;
    }
    return !bRequireIdle || (pExec != nullptr && pExec->m_pPool != nullptr && pExec->m_pPool->PendingCount() == 0);
}

/// @brief 按线程亲和派发一个任务体：就地内联 / 投递执行器。
///
/// 判定见 `ShouldInline`（就地）与 `PostToHandle`（投递）。
///
/// @param eAffinity 亲和三档。
/// @param pExec 目标执行器句柄（调用方已用 `ResolveExecHandle` 解析好）。
/// @param fnTask 任务体（按值接收：就地执行或移动投递）。
/// @return true 已就地执行 / 已投递；false 执行器不可用（调用方以 `kStopped` 收口本层）。
inline bool DispatchInlineOrPost(
    HandlerAffinity eAffinity, const std::shared_ptr<CExecutorHandle>& pExec, std::function<void()> fnTask)
{
    if (ShouldInline(eAffinity, pExec))
    {
        CInlineGuard guard;  // 深度 +1 / -1 成对（异常 / 提前 return 也不漏减）。
        fnTask();
        return true;
    }
    return PostToHandle(pExec, std::move(fnTask));
}

}  // namespace detail

/// @brief 异步执行器：工作线程池 + 投递入口。
///
/// 非模板类；起 promise 通过模板成员 NewPromise 完成（上下文类型由参数推导）。
class CAsyncExecutor
{
public:
    //================ Lifecycle ================

    // 创建执行器（线程数默认 1）。
    explicit CAsyncExecutor(size_t nThreadCount = 1);

    // 不可拷贝（拷贝会共享线程池，Stop 相互影响）。
    CAsyncExecutor(const CAsyncExecutor&) = delete;
    CAsyncExecutor& operator=(const CAsyncExecutor&) = delete;

    // 销毁执行器（停止线程池并等待已投递任务完成）。
    ~CAsyncExecutor();

    // 启动工作线程。
    bool Start();

    // 停止并等待任务完成（优雅关闭）。
    void Stop();

    // 是否正在运行。
    bool IsRunning() const;

    // 是否已停止（停止后拒绝新投递）。
    bool IsStopped() const;

    //================ Post ================

    // 投递无返回值任务（fire-and-forget）。
    bool Post(std::function<void()> fnTask);

    //================ Chain ================

    // 起 promise（等价 JS `new Promise(executor)`）：创建 promise 并投递首层。
    template <typename TContext>
    CPromise<TContext> NewPromise(const std::shared_ptr<TContext>& spContext, typename CPromise<TContext>::ThenHandler fnHandler,
        const CSourceLoc& loc = CSourceLoc());

    // 起 promise（对齐 JS `new Promise((resolve, reject) => ...)`）：由 fnExecutor 内部的 resolve / reject 兑现。
    template <typename TContext>
    CPromise<TContext> NewPromise(const std::shared_ptr<TContext>& spContext,
        const typename CPromise<TContext>::PromiseExecutor& fnExecutor, const CSourceLoc& loc = CSourceLoc());

    //================ Combine ================

    // 组合器（对齐 JS `Promise.all`）：全部兑现才兑现；任一拒绝立即以该拒绝码拒绝。
    template <typename TContext, typename... TChild>
    CPromise<TContext> WhenAll(const std::shared_ptr<TContext>& spContext, const TChild&... child);

    // 组合器（对齐 JS `Promise.allSettled`）：全部落定即兑现（恒兑现）。
    template <typename TContext, typename... TChild>
    CPromise<TContext> WhenAllSettled(const std::shared_ptr<TContext>& spContext, const TChild&... child);

    // 组合器（对齐 JS `Promise.race`）：首个落定者定结果（兑现 / 拒绝皆可）。
    template <typename TContext, typename... TChild>
    CPromise<TContext> WhenRace(const std::shared_ptr<TContext>& spContext, const TChild&... child);

    // 组合器（对齐 JS `Promise.any`）：首个兑现者定结果；全部拒绝才失败。
    template <typename TContext, typename... TChild>
    CPromise<TContext> WhenAny(const std::shared_ptr<TContext>& spContext, const TChild&... child);

    //================ Coroutine ================

    // 创建并启动协程（投递首次 Resume；返回 shared_ptr 管理生命周期）。
    template <typename TCoroutine, typename... TArgs>
    std::shared_ptr<TCoroutine> CoStart(TArgs&&... args);

private:
    //================ Internal ================

    template <typename TContext>
    friend class CPromise;  // 取执行器句柄（起链 / 逐层亲和 / 通知投递）。

    template <typename TContext>
    friend class CCoroutine;  // 取执行器句柄 + 空闲判定（子 promise 投递 / 内联续接）。

    // 当前线程是否本执行器的工作线程（线程亲和判定；框架内部用）。
    bool IsInExecutorThread() const
    {
        return detail::IsInExecutorThread(m_pHandle);
    }

    // 执行器句柄（promise / 协程持有，生命周期加固用）。
    const std::shared_ptr<detail::CExecutorHandle>& Handle() const
    {
        return m_pHandle;
    }

    std::shared_ptr<detail::CExecutorHandle> m_pHandle;  ///< 执行器句柄（promise / 协程共享）。
    size_t m_nThreadCount;                               ///< 工作线程数。
};

//================ Combine ================

// 组合器（`exec.WhenAll` 一族：把多个子 promise 汇成一条**聚合链**）
//
// 对齐 JS `Promise.all` / `allSettled` / `race` / `any`：新造一条「由子 promise 的落定驱动」的
// 聚合链，与 `NewPromise` 同族的起链入口（完整语义文档见本节各入口定义）。
//
// 为什么实现能放在本文件（这里只前置声明了 `CPromise`）：对 `CPromise` 的每一处使用都落在
// **模板的依赖上下文**里（`promiseChild.OnSettled(...)`、限定名
// `CPromise<TContext>::New`、按值返回尚不完整的 `CPromise<TContext>`）—— 名字查找与类型完备性
// 检查都推迟到**实例化点**，而实例化发生在调用方 TU（那时它必然已经 include 了 `Promise.h`）。
//
// 聚合状态是纯状态（不碰上下文类型），所以跨模块 / 跨上下文类型的分支能汇到同一个聚合上；
// 子 promise 的落定可能发生在**任意线程**上，故「锁内判定、锁外收口」。框架不提供取消：
// fail-fast / race 收口后，其余子 promise 照旧跑完（结果被忽略）。


namespace detail {

/// @brief 组合器策略（`WhenAll` / `WhenAllSettled` / `WhenRace` / `WhenAny` 四档）。
enum GatherPolicy
{
    kGatherAll = 0,         ///< 对齐 JS `Promise.all`：全部兑现才兑现；任一拒绝立即以该拒绝码拒绝。
    kGatherAllSettled = 1,  ///< 对齐 JS `Promise.allSettled`：全部落定即兑现（不看各分支成败）。
    kGatherRace = 2,        ///< 对齐 JS `Promise.race`：首个落定者定结果（兑现 / 拒绝皆可）。
    kGatherAny = 3          ///< 对齐 JS `Promise.any`：首个兑现者兑现；全部拒绝才以首个拒绝码拒绝。
};

/// @brief 一处子 promise 都没有时的收口结果（对齐 JS）。
///
/// `all` / `allSettled` 视为成功（没有要等的东西）；`race` / `any` 不可能有结果 →
/// 以 `kRejected` 拒绝（否则聚合链永久 pending，`Await()` 会死等）。
///
/// @param ePolicy 策略（GatherPolicy 四档）。
/// @return 空集合应立即采用的最终结果。
inline CPromiseResult ResolveEmptyGather(GatherPolicy ePolicy)
{
    return (ePolicy == kGatherAll || ePolicy == kGatherAllSettled) ? CPromiseResult::Resolve()
                                                                   : CPromiseResult::Reject(kRejected);
}

/// @brief 组合器聚合状态（把 N 个子 promise 的落定折算成「一条聚合链」的落定）。
///
/// 与 `CPromiseState` 一样是**非模板**的纯状态：它只关心子 promise 的成败与拒绝码，
/// 完全不碰上下文类型 —— 所以「跨模块（不同 TContext）的分支汇到同一个聚合」不需要额外机制。
///
/// 收口动作由调用方以 resolve / reject 传入（聚合链的层状态由它们 settle）。
class CGatherState
{
public:
    /// @brief 创建聚合状态。
    ///
    /// @param ePolicy 策略（GatherPolicy 四档）。
    /// @param nTotal 子 promise 总数（> 0；空集合由调用方在收口前先处理）。
    /// @param fnResolve 兑现聚合链的当前层。
    /// @param fnReject 拒绝聚合链的当前层。
    CGatherState(
        GatherPolicy ePolicy, int nTotal, const std::function<void()>& fnResolve, const std::function<void(int)>& fnReject)
        : m_ePolicy(ePolicy),
          m_nPending(nTotal),
          m_bRejectSeen(false),
          m_bDone(false),
          m_nFirstRejectCode(kRejected),
          m_fnResolve(fnResolve),
          m_fnReject(fnReject)
    {}

    /// @brief 一个子 promise 落定（可能被不同线程并发调用）。
    ///
    /// @param result 子 promise 的最终结果。
    void OnChildSettled(const CPromiseResult& result)
    {
        bool bResolve = false;
        bool bReject = false;
        int nCode = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bDone)
            {
                return;  // 已收口：迟到的子 promise 直接忽略（框架不取消它们）。
            }

            --m_nPending;
            if (!result.IsFulfilled() && !m_bRejectSeen)
            {
                m_bRejectSeen = true;
                m_nFirstRejectCode = result.Code();  // `any` 在全部拒绝时用它收口。
            }

            switch (m_ePolicy)
            {
                case kGatherAll:
                    // 任一拒绝 → 立即收口（及时失败）；全部兑现 → 才兑现。
                    if (result.IsFulfilled())
                    {
                        m_bDone = (m_nPending == 0);
                        bResolve = m_bDone;
                    }
                    else
                    {
                        m_bDone = true;
                        bReject = true;
                        nCode = result.Code();
                    }
                    break;
                case kGatherAllSettled:
                    // 成败都算数：全部落定即兑现（各分支的成败由调用方从子句柄读）。
                    m_bDone = (m_nPending == 0);
                    bResolve = m_bDone;
                    break;
                case kGatherRace:
                    // 首个落定者定结果（兑现 / 拒绝都算）。
                    m_bDone = true;
                    bResolve = result.IsFulfilled();
                    bReject = !bResolve;
                    nCode = m_nFirstRejectCode;
                    break;
                case kGatherAny:
                default:
                    // 首个兑现者定结果；全部拒绝才拒绝（对齐 JS Promise.any）。
                    if (result.IsFulfilled())
                    {
                        m_bDone = true;
                        bResolve = true;
                    }
                    else if (m_nPending == 0)
                    {
                        m_bDone = true;
                        bReject = true;
                        nCode = m_nFirstRejectCode;
                    }
                    break;
            }
        }

        // 锁外收口：settle 聚合层会触发它的下一层（可能就地执行，持锁调用有死锁风险）。
        if (bResolve && m_fnResolve)
        {
            m_fnResolve();
        }
        if (bReject && m_fnReject)
        {
            m_fnReject(nCode);
        }
    }

private:
    std::mutex m_mutex;                   ///< 保护下面的计数（子 promise 在不同线程上落定）。
    GatherPolicy m_ePolicy;               ///< 策略（GatherPolicy 四档）。
    int m_nPending;                       ///< 尚未落定的子 promise 数。
    bool m_bRejectSeen;                   ///< 是否已见过拒绝（`any` 收口要用首个拒绝码）。
    bool m_bDone;                         ///< 聚合是否已收口（收口后忽略迟到的子 promise）。
    int m_nFirstRejectCode;               ///< 首个拒绝码（m_bRejectSeen 为 true 时有效）。
    std::function<void()> m_fnResolve;    ///< 兑现聚合链的当前层。
    std::function<void(int)> m_fnReject;  ///< 拒绝聚合链的当前层。
};

/// @brief 把「子 promise 落定 → 聚合状态」登记到子 promise 上（组合器唯一的登记路径）。
///
/// 只登记回调、不阻塞任何线程（子 promise 已落定时由 `OnSettled` 的送达保证立即触发）。
///
/// @param pGather 聚合状态。
/// @param promiseChild 子 promise（上下文类型任意；恒有效）。
template <typename TChildContext>
void BindChildGather(const std::shared_ptr<CGatherState>& pGather, const CPromise<TChildContext>& promiseChild)
{
    // 通知恒送达（没有返回值）→ 子链落定即计入聚合。
    promiseChild.OnSettled(
        [pGather](CPromiseResult childResult)
        {
            pGather->OnChildSettled(childResult);
        });
}

/// @brief 把一个子 promise 追加到登记动作列表（`Gather` 摊平参数包用）。
///
/// C++11 的 lambda 捕获列表不能展开参数包，所以先给每个子 promise 生成一个登记动作
/// （`std::vector` 收集），再由聚合链的 executor 逐个执行。
///
/// @param vecOut 登记动作列表（追加到末尾）。
/// @param promiseChild 子 promise。
template <typename TChildContext>
void AppendGatherBindings(
    std::vector<std::function<void(const std::shared_ptr<CGatherState>&)> >& vecOut, const CPromise<TChildContext>& promiseChild)
{
    vecOut.push_back(
        [promiseChild](const std::shared_ptr<CGatherState>& pGather)
        {
            BindChildGather(pGather, promiseChild);
        });
}

/// @brief 把一组子 promise 追加到登记动作列表（数量运行时确定时用）。
///
/// 与标量版同名重载，所以 `exec.WhenAll(spCtx, pA, vecBranches, pB)` 这种「标量 + 列表混用」
/// 也能直接写。
///
/// @param vecOut 登记动作列表（追加到末尾）。
/// @param vecChild 子 promise 列表（同一上下文类型）。
template <typename TChildContext>
void AppendGatherBindings(std::vector<std::function<void(const std::shared_ptr<CGatherState>&)> >& vecOut,
    const std::vector<CPromise<TChildContext> >& vecChild)
{
    for (size_t i = 0; i < vecChild.size(); ++i)
    {
        AppendGatherBindings(vecOut, vecChild[i]);
    }
}

/// @brief 组合器的统一实现（四个 `When*` 只差一个策略）。
///
/// 参数可为单个子 promise（`CPromise<任意上下文>`），也可为 `std::vector<CPromise<同上下文>>`
/// （数量运行时确定时用），两者可混用 —— 展开后按参数顺序登记。
///
/// 聚合链的当前层用 `exec.NewPromise(spCtx, executor)` 造（由外部 settle）：executor 里只做「逐个登记子 promise」，
/// 不做重活、不阻塞 —— 子 promise 落在哪个线程都不会占住聚合链的线程。
///
/// 一处子 promise 都没有时直接在此收口（对齐 JS）：`all` / `allSettled` 立即兑现；
/// `race` / `any` 不可能有结果 → 立即以 `kRejected` 拒绝（否则永久 pending，死等）。
///
/// @note 本函数在此头文件里定义（见本节开头的说明），**实例化需要 `CPromise` 完整类型** ——
///       调用方 TU 需 include "Async/Promise.h"（拿 promise 句柄时本来就会 include）。
///
/// @tparam TContext 聚合 promise 的上下文类型。
/// @tparam TChild 子 promise 类型 / 子 promise 列表类型。
/// @param executor 聚合链的执行器。
/// @param spContext 聚合 promise 的共享上下文。
/// @param ePolicy 策略（GatherPolicy 四档）。
/// @param child 子 promise，或 `std::vector<CPromise<同上下文>>`。
/// @return 聚合 promise 句柄（pending；由子 promise 的落定驱动）。
template <typename TContext, typename... TChild>
CPromise<TContext> Gather(
    CAsyncExecutor& executor, const std::shared_ptr<TContext>& spContext, GatherPolicy ePolicy, const TChild&... child)
{
    std::vector<std::function<void(const std::shared_ptr<CGatherState>&)> > vecBindings;
    const int nUnused[] = {0, (AppendGatherBindings(vecBindings, child), 0)...};
    (void)nUnused;

    if (vecBindings.empty())
    {
        // 一处子 promise 都没有：按策略直接收口（语义只有 `ResolveEmptyGather` 一处）。
        const CPromiseResult emptyResult = ResolveEmptyGather(ePolicy);
        return executor.NewPromise(
            spContext, typename CPromise<TContext>::PromiseExecutor(
                           [emptyResult](const std::function<void()>& fnResolve, const std::function<void(int)>& fnReject)
                           {
                               if (emptyResult.IsFulfilled())
                               {
                                   fnResolve();
                                   return;
                               }
                               fnReject(emptyResult.Code());
                           }));
    }

    const int nTotal = static_cast<int>(vecBindings.size());
    return executor.NewPromise(spContext,
        typename CPromise<TContext>::PromiseExecutor(
            [ePolicy, nTotal, vecBindings](const std::function<void()>& fnResolve, const std::function<void(int)>& fnReject)
            {
                const std::shared_ptr<CGatherState> pGather =
                    std::make_shared<CGatherState>(ePolicy, nTotal, fnResolve, fnReject);
                for (size_t i = 0; i < vecBindings.size(); ++i)
                {
                    vecBindings[i](pGather);  // 登记动作恒非空。
                }
            }));
}

}  // namespace detail

/// @brief 组合器（对齐 JS `Promise.all`）：等一组子 promise **全部兑现**；
///        任一拒绝 → 立即以该拒绝码拒绝（其余分支继续跑完，结果被忽略）。
///
/// 用途：并行分支 / 并行调用多个模块（子 promise **可跨上下文类型**），全部完成后继续本链。
/// 聚合 promise 只关心分支成败，**不传值**：数据请让各分支写进自己的共享上下文
/// （同上下文时共用一个实例即可）。
///
/// 语义：
///  - 全部兑现 → 聚合兑现；
///  - **任一拒绝 → 立即以该拒绝码拒绝**（对齐 JS：及时失败；其余分支继续跑完，结果被忽略）；
///  - 已落定的子 promise 直接计入；
///  - 一处子 promise 都没给 → 立即兑现。
///
/// @note 本执行器只用于**聚合 promise 自己的层**（`.Then(...)` 等）；各子 promise 仍跑在
///       它们各自的执行器上。组合器只有这四个执行器入口（没有 `CPromise` 成员形态）。
///
/// @tparam TContext 聚合 promise 的上下文类型（由 spContext 推导）。
/// @tparam TChild 子 promise 类型 / 子 promise 列表类型。
/// @param spContext 聚合 promise 的共享上下文（**必传**；与其他起链入口一致，框架不代建）。
/// @param child 子 promise，或 `std::vector<CPromise<同上下文>>`（数量运行时确定）；两者可混用。
/// @return 聚合 promise 句柄（pending；由子 promise 的落定驱动）。
template <typename TContext, typename... TChild>
CPromise<TContext> CAsyncExecutor::WhenAll(const std::shared_ptr<TContext>& spContext, const TChild&... child)
{
    return detail::Gather(*this, spContext, detail::kGatherAll, child...);
}

/// @brief 组合器（对齐 JS `Promise.allSettled`）：等一组子 promise **全部落定**后兑现（恒兑现）。
///
/// 与 `WhenAll` 的差别：**不因任何分支被拒绝而失败** —— 「并行发起 N 件事，全部有结论后再继续」
/// 用它（典型：批量通知 / 收尾清理 / 并行上报，个别失败不影响整体）。
///
/// 各分支的成败在本框架里没有值通道，调用方自己读：此时各子句柄都已落定，
/// `child.Await()` 会立即返回该分支的 `CPromiseResult`（不阻塞），或事先挂 `OnSettled`。
///
/// @note 其余语义（跨上下文类型、空集合立即兑现）同 `WhenAll`。
///
/// @tparam TContext 聚合 promise 的上下文类型（由 spContext 推导）。
/// @tparam TChild 子 promise 类型 / 子 promise 列表类型。
/// @param spContext 聚合 promise 的共享上下文（**必传**）。
/// @param child 子 promise，或 `std::vector<CPromise<同上下文>>`（数量运行时确定）；两者可混用。
/// @return 聚合 promise 句柄（恒兑现；由子 promise 的落定驱动）。
template <typename TContext, typename... TChild>
CPromise<TContext> CAsyncExecutor::WhenAllSettled(const std::shared_ptr<TContext>& spContext, const TChild&... child)
{
    return detail::Gather(*this, spContext, detail::kGatherAllSettled, child...);
}

/// @brief 组合器（对齐 JS `Promise.race`）：**首个落定**的子 promise 定结果（兑现 / 拒绝皆可）。
///
/// 用法：并行发起多条路径，谁先有结论就用谁（典型：主链路 + 备用链路取先到者）。
/// 与 `WhenAny` 的差别：race 里「先失败」也算结论，any 只认「先兑现」。
///
/// @warning 「先到」取决于各子 promise 实际落定的时刻与送达顺序（跨执行器时不保证与参数顺序一致）。
///          框架不取消落败的分支，它们会继续跑完（结果被忽略）。
///
/// @note 其余语义同 `WhenAll`；空集合 → 立即以 `kRejected` 拒绝（race 无结果可用）。
///
/// @tparam TContext 聚合 promise 的上下文类型（由 spContext 推导）。
/// @tparam TChild 子 promise 类型 / 子 promise 列表类型。
/// @param spContext 聚合 promise 的共享上下文（**必传**）。
/// @param child 子 promise，或 `std::vector<CPromise<同上下文>>`（数量运行时确定）；两者可混用。
/// @return 聚合 promise 句柄（由首个落定的子 promise 驱动）。
template <typename TContext, typename... TChild>
CPromise<TContext> CAsyncExecutor::WhenRace(const std::shared_ptr<TContext>& spContext, const TChild&... child)
{
    return detail::Gather(*this, spContext, detail::kGatherRace, child...);
}

/// @brief 组合器（对齐 JS `Promise.any`）：**首个兑现**的子 promise 定结果；全部拒绝才失败。
///
/// 用法：多条等价路径取「第一个成功的」（典型：多副本 / 多后端取先返回成功者）；
/// 全部失败时以**首个拒绝码**收口（JS 是 AggregateError，本框架用码表达）。
///
/// @note 其余语义同 `WhenAll`；空集合 → 立即以 `kRejected` 拒绝（不可能有兑现者）。
///
/// @tparam TContext 聚合 promise 的上下文类型（由 spContext 推导）。
/// @tparam TChild 子 promise 类型 / 子 promise 列表类型。
/// @param spContext 聚合 promise 的共享上下文（**必传**）。
/// @param child 子 promise，或 `std::vector<CPromise<同上下文>>`（数量运行时确定）；两者可混用。
/// @return 聚合 promise 句柄（由首个兑现的子 promise 驱动）。
template <typename TContext, typename... TChild>
CPromise<TContext> CAsyncExecutor::WhenAny(const std::shared_ptr<TContext>& spContext, const TChild&... child)
{
    return detail::Gather(*this, spContext, detail::kGatherAny, child...);
}

}  // namespace async
}  // namespace common
