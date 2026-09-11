#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/PromiseResult.h"
#include "Async/PromiseTypes.h"
#include "Async/SourceLoc.h"

// ====================================================================
// CPromise —— 异步 promise（命名与语义对齐 JS 的 Promise / async-await）
//
// 本框架不支持在层与层之间传递任意值：每层只产出「已兑现 / 已拒绝」
// （CPromiseResult），数据统一放在共享上下文（std::shared_ptr<TContext>）。
//
// 因此处理器（handler）签名固定：
//
//     CPromiseResult handler(CPromiseResult upResult,              // 上一层结果
//                            const std::shared_ptr<TContext>& spCtx); // 共享上下文
//
// JS 对照：
//
//   new Promise(executor)          →  CPromise<Ctx> p(exec, spCtx, StepA);   // 构造即起链
//                                     auto p = exec.NewPromise(spCtx, StepA); // 等价写法
//   promise.then(onFulfilled)      →  p.Then(StepB);        // 兑现时执行，拒绝直接透传
//   promise.catch(onRejected)      →  p.Catch(StepRollback);// 拒绝时执行，Resolve() 即恢复
//   promise.finally(onFinally)     →  p.Finally(StepLog);   // 无论成败都执行，不改结果
//   await promise / .then(…)       →  p.Await()             // 阻塞等待（返回 CPromiseResult）
//   promise 已 settle              →  p.IsSettled()
//   resolve() / reject(reason)     →  CPromiseResult::Resolve() / CPromiseResult::Reject(码)
//   fulfilled / rejected           →  result.IsFulfilled() / result.IsRejected()
//   new Promise((resolve, reject)    →  CPromise<Ctx>::New(exec, spCtx, executor, ASYNC_LOC);
//     => { … 回调里 resolve()/reject()… })   // 由外部（其他模块 / 回调）兑现或拒绝本 promise
//   then(onFulfilled 返回 promise)  →  p.ThenPromise(FnFactory);   // 等子 promise（flatten）
//
// 语义要点：
//  - then / catch / finally 都返回「指向新一层的 promise」（与 JS 一致，链式可读）；
//  - 失败即停：then 层在上一层被拒绝时不执行，拒绝原因沿链透传；
//  - catch 层可恢复：返回 Resolve() 即吞掉拒绝，链从本层之后继续；
//  - finally 层只做收尾（回滚 / 清理 / 日志），**忽略返回值、原样透传上层结果**；
//  - 层内异常 → 本层被拒绝（kException），不向调用方抛出；
//  - 首层投递一次；后续层带「线程亲和」：已在本链执行器线程上就地级联（超过 kMaxInlineDepth
//    改投递防爆栈），否则（跨执行器 / 跨模块）投递回本链执行器 —— 保证每层都在本链执行器线程上。
//
// 嵌套用法（异步里再起异步）：
//   ① 协程内 await（推荐，非阻塞挂起）：
//        CO_AWAIT(NewPromise(StepSub));          // 子 promise（同上下文）
//        CO_AWAIT(pChild->AsPromise());          // 子协程
//        协程还能 await **别的上下文类型**的 promise（跨流程嵌套）：
//        CO_AWAIT(exec.NewPromise(spDbCtx, StepQuery, ASYNC_LOC));
//   ② 并行嵌套：CO_AWAIT_ALL(a, b, c)，其中 a/b/c 各自可以是多步子 promise（a.Then(...)）；
//   ③ 层内嵌套（非阻塞）：层里起子 promise，由它的 OnSettled 回调接着写上下文 / 起后续；
//   ④ 层内嵌套（阻塞）：层里 sub.Await() —— 会占住一个工作线程，**线程池必须还有空闲
//      worker**，否则死锁（单线程执行器必死），只适合子流程很短且并发余量充足的场合。
//
// ⑤ 跨模块 / 跨上下文组合（**纯异步、零阻塞、不需要协程**）：把别的 promise 桥接进本流程 ——
//      ① 用 `CPromise<CMyCtx>::New(exec, spCtx, executor)` 造一条「由外部 settle」的 promise
//         （executor 里发起别的模块的调用，在其 OnSettled 回调里 resolve() / reject(码)）；
//      ② 用 `p.ThenPromise([&]{ return bridgePromise; })` 把它接进本流程（then 的 promise 版）。
//    本流程的最终结果 = 含跨模块子流程的完整结果，全程不阻塞任何线程。
//
// 并发注意：并行/嵌套的子 promise 若共用同一份共享上下文，请让各分支只写**不同字段**
// （或自行加同步）—— 框架只保证「同一条链的层顺序执行」，跨链并发由调用方负责。
//
// 用法：
// @code
// struct CLoginContext                     // 一次流程的共享数据（TContext）。
// {
//     std::string strAccount;
//     std::string strToken;
// };
//
// CPromiseResult StepReadParam(CPromiseResult upResult, const std::shared_ptr<CLoginContext>& spCtx)
// {
//     if (upResult.IsRejected()) { return upResult; }     // 上一层被拒绝：透传
//     spCtx->strAccount = ReadAccountFromRequest();
//     return spCtx->strAccount.empty() ? CPromiseResult::Reject(kCodeNoAccount)
//                                      : CPromiseResult::Resolve();
// }
//
// common::async::CAsyncExecutor exec(2);
// exec.Start();
//
// std::shared_ptr<CLoginContext> spCtx = std::make_shared<CLoginContext>();
// common::async::CPromise<CLoginContext> p =
//     exec.NewPromise(spCtx, StepReadParam, ASYNC_LOC)     // 起链（首层）
//         .Then(StepVerify, ASYNC_LOC)                     // 兑现路径
//         .Catch(StepRollback, ASYNC_LOC)                  // 拒绝路径（回滚，可恢复）
//         .Finally(StepWriteLog, ASYNC_LOC);               // 收尾（成败都跑）
// p.OnSettled([](common::async::CPromiseResult r) { /* 跑完通知 */ });
// common::async::CPromiseResult r = p.Await();             // 阻塞等待结果
// (void)r;
//
// 需要「先把链完全搭好、再开跑」时（等价 build-then-start）：
//   no::CPromise<CLoginContext> q = exec.BuildPromise(spCtx)   // 追加层只登记，不投递
//       .Then(StepReadParam, ASYNC_LOC)
//       .Then(StepVerify, ASYNC_LOC);
//   q.Start();                     // 此刻才投递首层（幂等；漏写则首次 Await 自动启动）
//   q.Await();
// @endcode
// ====================================================================

namespace common {
namespace async {

template <typename TContext>
class CPromise;
template <typename TContext>
class CCoroutine;

namespace detail {

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

/// @brief promise 状态（对应 JS 中「每个 then 返回的新 promise」的状态）。
///
/// 一道 promise 链由若干状态串成，一个状态对应一层。状态是单向开关：
/// pending → settled（fulfilled | rejected），首次 Settle 生效；之前登记的
/// 处理器（handler）随后在锁外触发（链的逐层推进就在这条调用路径上级联完成）。
class CPromiseState
{
   public:
    /// 处理器：接收上一层结果。
    using Handler = std::function<void(const CPromiseResult&)>;

    /// @brief 创建状态（pending）。
    CPromiseState() : m_bSettled(false), m_result()
    {}

    /// @brief settle 本状态并触发处理器（锁外调用处理器，防重入死锁）。
    ///
    /// 仅首次生效；先唤醒等待者，再按注册顺序在锁外调用所有处理器。
    /// 处理器在调用方（结算）线程上被触发；若它是「层处理器」，再由 `RunHandler`
    /// 按线程亲和决定就地执行（已在本链执行器线程）还是投递回本链执行器。
    ///
    /// @param result 本层最终结果（已兑现 / 已拒绝）。
    void Settle(const CPromiseResult& result)
    {
        std::vector<Handler> vecHandlers;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bSettled.load(std::memory_order_relaxed))
            {
                return;  // 单向开关：只 settle 一次。
            }
            m_result = result;
            m_bSettled.store(true, std::memory_order_relaxed);  // 锁内写；relaxed 即可。
            vecHandlers.swap(m_vecHandlers);
        }

        // 支持多线程等待同一 promise（并发 Await）：notify_all 唤醒所有等待者。
        m_cv.notify_all();

        for (size_t i = 0; i < vecHandlers.size(); ++i)
        {
            if (vecHandlers[i])
            {
                vecHandlers[i](result);
            }
        }
    }

    /// @brief 登记处理器；本状态已 settled 时投递到执行器异步触发（不阻塞调用方）。
    ///
    /// 层处理器（then / catch / finally / thenPromise）用本接口：执行器不可用时返回 false，
    /// 由调用方以 `kStopped` 收口本层（“停了的执行器不再跑新层”）。
    ///
    /// @param pHandle 执行器句柄（已 settled 时投递用）。
    /// @param fnHandler 处理器（按值接收，登记时移动存储避免拷贝）。
    /// @return true 已登记或已投递；false 已 settled 但执行器不可用（处理器不执行）。
    bool AddHandler(const std::shared_ptr<CExecutorHandle>& pHandle, Handler fnHandler)
    {
        bool bFireNow = false;
        CPromiseResult result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_bSettled.load(std::memory_order_relaxed))
            {
                m_vecHandlers.push_back(std::move(fnHandler));
                return true;  // pending：已登记，settle 时触发。
            }
            bFireNow = true;
            result = m_result;
        }

        if (bFireNow && fnHandler)
        {
            // 已 settled：投递到执行器异步执行（与 JS 一致），保持调用方不阻塞。
            std::function<void()> fnRun = [fnHandler, result]()
            {
                fnHandler(result);
            };
            if (PostToHandle(pHandle, std::move(fnRun)))
            {
                return true;
            }
            return false;  // 已 settled 但执行器不可用。
        }
        return true;
    }

    /// @brief 登记「保证送达」的 settled 通知：即使执行器不可用也一定执行。
    ///
    /// 与 `AddHandler` 的差别只在兜底：执行器不可用（对方模块已停止 / 拒绝投递）时
    /// **在调用线程上就地执行**，绝不丢弃处理器。
    ///
    /// 用途：`OnSettled` —— 通知是「观测/收尾」语义，调用方不该为了“对方模块已停止”
    /// 再写一遍兜底代码；否则手写桥接漏检返回值就会让本层永久 pending、上层 `Await()` 死等。
    ///
    /// 就地执行不会递归加深：通知里通常只是 settle 本层，而该层后续处理器走 `RunHandler`，
    /// 执行器不可用时以 `kStopped` 收口，链会立刻结束。
    ///
    /// @param pHandle 执行器句柄（可用时投递，保证不阻塞调用方）。
    /// @param fnHandler 通知处理器（按值接收）。
    void AddSettledHandler(const std::shared_ptr<CExecutorHandle>& pHandle, Handler fnHandler)
    {
        bool bFireNow = false;
        CPromiseResult result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_bSettled.load(std::memory_order_relaxed))
            {
                m_vecHandlers.push_back(std::move(fnHandler));
                return;  // pending：已登记，settle 时触发（在结算线程上）。
            }
            bFireNow = true;
            result = m_result;
        }

        if (!bFireNow || !fnHandler)
        {
            return;
        }

        // 已 settled：优先投递到执行器（不阻塞调用方）；不可用则就地送达。
        std::function<void()> fnRun = [fnHandler, result]()
        {
            fnHandler(result);
        };
        if (PostToHandle(pHandle, fnRun))  // 注意：按值传参（拷贝）——失败时 fnRun 仍可用。
        {
            return;
        }

        ++InlineDepth();  // 与其它内联路径共用深度计数（防极端嵌套）。
        fnRun();
        --InlineDepth();
    }

    /// @brief 阻塞等待本状态 settle（先短自旋，超时再阻塞等待）。
    ///
    /// @return 本层最终结果（已兑现 / 已拒绝）。
    CPromiseResult Await()
    {
        // 短自旋（relaxed 读仅作宽松提示）；最终由锁内条件判定。
        const auto spinDeadline = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
        while (!m_bSettled.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < spinDeadline)
        {
            std::this_thread::yield();
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]()
        {
            return m_bSettled.load(std::memory_order_relaxed);
        });
        return m_result;
    }

    /// @brief 本状态是否已 settled（兑现或拒绝）。
    bool IsSettled() const
    {
        return m_bSettled.load(std::memory_order_relaxed);
    }

    /// @brief 设置本层的注册点源码位置（调试用；发布构建为空操作）。
    ///
    /// @param loc 源码位置（建议传 ASYNC_LOC）。
    void SetLoc(const CSourceLoc& loc)
    {
#if defined(ASYNC_DEBUG_TRACE)
        m_loc = loc;
#else
        (void)loc;
#endif
    }

    /// @brief 获取本层注册点源码位置（发布构建恒为空）。
    CSourceLoc Loc() const
    {
#if defined(ASYNC_DEBUG_TRACE)
        return m_loc;
#else
        return CSourceLoc();
#endif
    }

   private:
    std::mutex m_mutex;                  ///< 保护结果与处理器列表。
    std::condition_variable m_cv;        ///< 通知等待者。
    std::vector<Handler> m_vecHandlers;  ///< 处理器列表（pending 时登记）。
    std::atomic<bool> m_bSettled;        ///< 是否已 settled（自旋读用）。
    CPromiseResult m_result;             ///< 最终结果（settled 后有效）。
#if defined(ASYNC_DEBUG_TRACE)
    CSourceLoc m_loc;  ///< 注册点源码位置（调试用）。
#endif
};

/// @brief 延迟启动状态（`BuildPromise` 建链用，整条链共享）。
///
/// 普通链（`NewPromise` / 构造函数）`bDeferred == false`：行为完全不变（首层立即投递）。
/// 延迟链：追加层只**登记**，首层动作暂存在 `fnLaunch`，由 `Start()` 才投递 ——
/// 于是「链跑起来」与「挂层」分开，跨模块续接的登记时机不再与执行赛跑。
struct CLaunchState
{
    bool bDeferred;                            ///< 是否延迟启动链。
    bool bStarted;                             ///< 是否已启动（Start 幂等）。
    std::function<void()> fnLaunch;            ///< 首层启动动作（Start 时投递到执行器执行）。
    std::shared_ptr<CPromiseState> pFirst;     ///< 首层状态（启动无法投递时以 kStopped 收口它）。
    std::shared_ptr<CExecutorHandle> pTarget;  ///< 启动投递的目标执行器（为空 = 本链执行器）。

    CLaunchState() : bDeferred(false), bStarted(false)
    {}
};

/// @brief promise 共享核心：执行器句柄 + 共享上下文。
///
/// 一条链的所有层共用同一个核心（同一上下文 + 同一执行器），句柄持有者彼此
/// 保活（执行器析构后链仍安全跑完）。
template <typename TContext>
class CPromiseCore
{
   public:
    /// @brief 创建核心。
    ///
    /// @param pHandle 执行器句柄（可为空：无效 promise）。
    /// @param spContext 共享上下文（可为空：首次 GetContext() 时懒创建）。
    CPromiseCore(const std::shared_ptr<CExecutorHandle>& pHandle, const std::shared_ptr<TContext>& spContext)
        : m_pHandle(pHandle), m_spContext(spContext)
    {}

    /// @brief 共享上下文（懒创建，恒非空）。
    ///
    /// 外部传入上下文时直接返回该实例；未传入时首次调用创建（默认构造）。
    std::shared_ptr<TContext> Context() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_spContext)
        {
            m_spContext = std::make_shared<TContext>();
        }
        return m_spContext;
    }

    /// @brief 执行器句柄（投递用）。
    const std::shared_ptr<CExecutorHandle>& Handle() const
    {
        return m_pHandle;
    }

    /// @brief 绑定执行器句柄（协程 Start 时注入）。
    ///
    /// @param pHandle 执行器句柄。
    void SetHandle(const std::shared_ptr<CExecutorHandle>& pHandle)
    {
        m_pHandle = pHandle;
    }

    /// @brief 延迟启动状态（懒创建，整条链共享）。
    ///
    /// @return 启动状态（非空）。
    std::shared_ptr<CLaunchState> Launch() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_pLaunch)
        {
            m_pLaunch = std::make_shared<CLaunchState>();
        }
        return m_pLaunch;
    }

   private:
    mutable std::mutex m_mutex;                       ///< 保护上下文与启动状态的懒创建。
    std::shared_ptr<CExecutorHandle> m_pHandle;       ///< 执行器句柄。
    mutable std::shared_ptr<TContext> m_spContext;    ///< 共享上下文。
    mutable std::shared_ptr<CLaunchState> m_pLaunch;  ///< 延迟启动状态（BuildPromise 链用）。
};

/// @brief 处理器模式（对应 JS 的 then / catch / finally）。
enum HandlerMode
{
    kModeThen = 0,    ///< then(onFulfilled)：上一层兑现时执行；被拒绝则直接透传（失败即停）。
    kModeCatch = 1,   ///< catch(onRejected)：上一层被拒绝时执行；已兑现则直接透传。
    kModeFinally = 2  ///< finally(onFinally)：无论兑现或拒绝都执行；忽略返回值，透传上层结果。
};

/// @brief 层处理器的执行线程偏好（线程亲和，默认 `kAffinityChain`）。
enum HandlerAffinity
{
    kAffinityChain = 0,  ///< 默认：本链执行器线程（同执行器内联；跨执行器投递回本链执行器）。
    kAffinityInline = 1,  ///< 就地：在「结算本层的那条线程」上执行（不投递，不要求线程亲和）。
    kAffinityExecutor = 2  ///< 指定执行器：在给定执行器线程上执行（同线程内联，否则投递）。
};

/// @brief 构造「执行本层处理器」的任务体（捕获核心与状态保活）。
///
/// @param pCore promise 共享核心（上下文 + 执行器句柄）。
/// @param pState 本层状态（执行结果写入它）。
/// @param fnHandler 处理器（固定签名）。
/// @param upResult 上一层结果。
/// @param nMode 处理器模式（then / catch / finally）。
/// @return 任务体（在工作线程上执行处理器并 settle 本层状态）。
template <typename TContext>
std::function<void()> MakeHandlerRunner(const std::shared_ptr<CPromiseCore<TContext> >& pCore,
                                        const std::shared_ptr<CPromiseState>& pState,
                                        const ThenHandler<TContext>& fnHandler, const CPromiseResult& upResult,
                                        int nMode)
{
    return [pCore, pState, fnHandler, upResult, nMode]()
    {
        CPromiseResult result;
        try
        {
            const CPromiseResult own = fnHandler(upResult, pCore->Context());
            // finally：忽略处理器返回的成败，原样透传上一层结果（JS: finally 不改变结果）。
            result = (nMode == kModeFinally) ? upResult : own;
        }
        catch (...)
        {
            result = CPromiseResult::Reject(kException);  // 处理器抛异常 → 拒绝（finally 抛异常同样覆盖）。
        }
        pState->Settle(result);  // settle 本层 → 触发下一层（同执行器内联 / 跨执行器投递）。
    };
}

/// @brief 级联执行下一层（上一层刚 settle，当前在主调方线程上）。
///
/// 线程亲和：**只有当前线程已经是本链执行器的线程**时才就地内联（省一次入队 + 保序）；
/// 否则一律投递回本链执行器（典型场景：被调模块 settle 本链的层，本层就回到本模块线程执行）。
/// 内联深度也只在同一执行器线程内累加，跨模块不会涨栈。
///
/// @param pCore promise 共享核心。
/// @param pState 本层状态。
/// @param fnHandler 处理器。
/// @param upResult 上一层结果。
/// @param nMode 处理器模式。
/// @param nAffinity 执行线程偏好（默认本链执行器；`kAffinityInline` 就地；`kAffinityExecutor` 用 pTarget）。
/// @param pTarget 指定执行器句柄（`kAffinityExecutor` 时有效）。
template <typename TContext>
void RunHandler(const std::shared_ptr<CPromiseCore<TContext> >& pCore, const std::shared_ptr<CPromiseState>& pState,
                const ThenHandler<TContext>& fnHandler, const CPromiseResult& upResult, int nMode,
                int nAffinity = kAffinityChain, const std::shared_ptr<CExecutorHandle>& pTarget = nullptr)
{
    std::function<void()> fnRun = MakeHandlerRunner(pCore, pState, fnHandler, upResult, nMode);

    // 选本层的执行器：默认本链执行器；kAffinityExecutor 用调用方指定的那个。
    const std::shared_ptr<CExecutorHandle> pExec =
        (nAffinity == kAffinityExecutor && pTarget != nullptr) ? pTarget : pCore->Handle();

    const bool bInline = (nAffinity == kAffinityInline) || IsInExecutorThread(pExec);
    if (bInline && InlineDepth() < kMaxInlineDepth)
    {
        ++InlineDepth();
        fnRun();
        --InlineDepth();
        return;
    }

    if (!PostToHandle(pExec, std::move(fnRun)))
    {
        pState->Settle(CPromiseResult::Reject(kStopped));  // 执行器不可用 → 本层被拒绝。
    }
}

/// @brief 投递执行首层（起链用：必须异步，不在起链线程上执行业务代码）。
///
/// @param pCore promise 共享核心。
/// @param pState 首层状态。
/// @param fnHandler 处理器。
/// @param upResult 上一层结果（首层恒为「已兑现」）。
/// @param nMode 处理器模式。
/// @param pExec 目标执行器句柄（为空则用 pCore 的执行器）。
template <typename TContext>
void PostHandler(const std::shared_ptr<CPromiseCore<TContext> >& pCore, const std::shared_ptr<CPromiseState>& pState,
                 const ThenHandler<TContext>& fnHandler, const CPromiseResult& upResult, int nMode,
                 const std::shared_ptr<CExecutorHandle>& pExec = nullptr)
{
    std::function<void()> fnRun = MakeHandlerRunner(pCore, pState, fnHandler, upResult, nMode);
    if (!PostToHandle(pExec != nullptr ? pExec : pCore->Handle(), std::move(fnRun)))
    {
        pState->Settle(CPromiseResult::Reject(kStopped));  // 执行器不可用 → 首层被拒绝。
    }
}

}  // namespace detail

/// @brief 异步 promise 句柄（浅句柄：拷贝共享同一条 promise 链的同一层）。
///
/// 一条 promise 链 = 共享核心（上下文 + 执行器）+ 一串状态（每层一个）。
/// 本类只是「指向某一层」的句柄：
///  - 构造时给出处理器 → 构造即起链（等价 `new Promise(executor)`）；
///  - `Then` / `Catch` / `Finally` 追加一层并返回指向新层的句柄（等价 JS 的
///    `then` / `catch` / `finally`）；
///  - `Await` / `OnSettled` / `IsSettled` 作用于句柄所指的那一层。
///
/// @tparam TContext 共享上下文类型（用户自定义的流程数据结构）。
template <typename TContext>
class CPromise
{
   public:
    /// 处理器类型（固定签名：上一层结果 + 共享上下文 → 本层结果）。
    using ThenHandler = detail::ThenHandler<TContext>;

    /// 兑现函数（对齐 JS `new Promise` 交给 executor 的 resolve）。
    using ResolveFn = std::function<void()>;

    /// 拒绝函数（对齐 JS `new Promise` 交给 executor 的 reject；reason 用错误码表达）。
    using RejectFn = std::function<void(int nCode)>;

    /// executor：对齐 JS `new Promise((resolve, reject) => { ... })` 的入参。
    ///
    /// 只应发起异步动作并注册回调，由回调调用 resolve() / reject(码) 兑现或拒绝本
    /// promise —— 非阻塞，不占工作线程。
    using PromiseExecutor = std::function<void(const ResolveFn& fnResolve, const RejectFn& fnReject)>;

    /// promise 工厂（ThenPromise 用）：返回一条需要等待的子 promise。
    using PromiseFactory = std::function<CPromise(const std::shared_ptr<TContext>& spContext)>;

    /// @brief 创建无效 promise（未绑定执行器；供成员声明 / 后续赋值用）。
    ///
    /// 无效 promise 上 Then / Catch / Finally 为空操作，Await() 返回被拒绝（kStopped）。
    CPromise() : m_pCore(), m_pState()
    {}

    /// @brief 创建 promise（未起链；上下文由本 promise 在首次取用时创建）。
    ///
    /// 随后第一次 Then / Catch / Finally 即首层（起点结果视为「已兑现」）。
    ///
    /// @param executor 执行器（起链与续接投递用）。
    explicit CPromise(CAsyncExecutor& executor)
        : m_pCore(std::make_shared<detail::CPromiseCore<TContext> >(executor.Handle(), std::shared_ptr<TContext>())),
          m_pState()
    {}

    /// @brief 创建 promise（未起链；使用外部已备好的共享上下文）。
    ///
    /// @param executor 执行器（起链与续接投递用）。
    /// @param spContext 共享上下文（外部持有；本 promise 所有层共用该实例）。
    CPromise(CAsyncExecutor& executor, const std::shared_ptr<TContext>& spContext)
        : m_pCore(std::make_shared<detail::CPromiseCore<TContext> >(executor.Handle(), spContext)), m_pState()
    {}

    /// @brief 创建并起链（等价 JS `new Promise(executor)`：executor 立即异步执行）。
    ///
    /// @param executor 执行器（起链与续接投递用）。
    /// @param fnHandler 首层处理器（固定签名）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    CPromise(CAsyncExecutor& executor, const ThenHandler& fnHandler, const CSourceLoc& loc = CSourceLoc())
        : m_pCore(std::make_shared<detail::CPromiseCore<TContext> >(executor.Handle(), std::shared_ptr<TContext>())),
          m_pState()
    {
        Append(fnHandler, loc, detail::kModeThen);
    }

    /// @brief 创建并起链（使用外部已备好的共享上下文）。
    ///
    /// @param executor 执行器（起链与续接投递用）。
    /// @param spContext 共享上下文（本 promise 所有层共用该实例）。
    /// @param fnHandler 首层处理器（固定签名）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    CPromise(CAsyncExecutor& executor, const std::shared_ptr<TContext>& spContext, const ThenHandler& fnHandler,
             const CSourceLoc& loc = CSourceLoc())
        : m_pCore(std::make_shared<detail::CPromiseCore<TContext> >(executor.Handle(), spContext)), m_pState()
    {
        Append(fnHandler, loc, detail::kModeThen);
    }

    /// @brief 是否有效（已绑定执行器）。
    bool IsValid() const
    {
        return m_pCore != nullptr && m_pState != nullptr;
    }

    /// @brief 本链是否「延迟启动」（`BuildPromise` 建的链）。
    bool IsDeferred() const
    {
        return m_pCore != nullptr && m_pCore->Launch()->bDeferred;
    }

    /// @brief 本链是否已启动（普通链恒为 true）。
    bool IsStarted() const
    {
        return m_pCore == nullptr || !m_pCore->Launch()->bDeferred || m_pCore->Launch()->bStarted;
    }

    /// @brief 启动「延迟链」：此刻才把首层投递到执行器（幂等；普通链调用无副作用）。
    ///
    /// - 延迟链（`BuildPromise`）在 `Start()` 之前**不跑任何业务代码**，追加层只做登记；
    /// - 未启动就叫 `Await()` 会自动 `Start()`（兜底，不会死等）；
    /// - 目标执行器不可用（已 Stop / 拒绝投递）→ 首层以 `kStopped` 收口，下游继续透传。
    void Start()
    {
        if (m_pCore == nullptr)
        {
            return;
        }

        const std::shared_ptr<detail::CLaunchState> pLaunch = m_pCore->Launch();
        if (!pLaunch->bDeferred || pLaunch->bStarted)
        {
            return;  // 非延迟链 / 已启动：幂等。
        }
        pLaunch->bStarted = true;

        std::function<void()> fnLaunch = pLaunch->fnLaunch;
        const std::shared_ptr<detail::CPromiseState> pFirst = pLaunch->pFirst;
        pLaunch->fnLaunch = nullptr;
        if (!fnLaunch || pFirst == nullptr)
        {
            return;  // 空链（没挂过任何层）：没有启动动作。
        }

        const std::shared_ptr<detail::CExecutorHandle> pExec =
            (pLaunch->pTarget != nullptr) ? pLaunch->pTarget : m_pCore->Handle();
        if (!detail::PostToHandle(pExec, std::move(fnLaunch)))
        {
            pFirst->Settle(CPromiseResult::Reject(kStopped));  // 执行器不可用。
        }
    }

    /// @brief 创建「由外部兑现 / 拒绝」的 promise（等价 JS `new Promise((resolve, reject) => ...)`）。
    ///
    /// 用途：把**其他模块 / 回调式**的异步接进本流程 —— executor 里发起调用并登记回调，
    /// 由对方的完成回调调 `fnResolve()` 兑现或 `fnReject(码)` 拒绝（非阻塞，不占 worker）。
    ///
    /// 与 JS 一致：executor **立即（同步）执行**，因此只应做「发起 + 登记回调」，不要做重活；
    /// 本 promise 在 settle 之前处于 pending，之后可正常 Then / Catch / Finally / ThenPromise。
    ///
    /// @param executor 执行器（起链与续接投递用）。
    /// @param spContext 共享上下文（本 promise 所有层共用该实例）。
    /// @param fnExecutor 执行体（对齐 JS executor：拿到 resolve / reject 句柄）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本 promise 的句柄（pending；由 fnExecutor 触发 settle）。
    static CPromise New(CAsyncExecutor& executor, const std::shared_ptr<TContext>& spContext,
                        const PromiseExecutor& fnExecutor, const CSourceLoc& loc = CSourceLoc())
    {
        CPromise promise(executor, spContext);
        promise.m_pState = std::make_shared<detail::CPromiseState>();  // 待定：等外部 settle。
        promise.m_pState->SetLoc(loc);

        const std::shared_ptr<detail::CPromiseCore<TContext> > pCore = promise.m_pCore;
        const std::shared_ptr<detail::CPromiseState> pState = promise.m_pState;

        // 延迟启动链（BuildPromise）：executor 不立即执行，等 Start() 后轮到本层再发起。
        if (pCore->Launch()->bDeferred)
        {
            pCore->Launch()->pFirst = pState;
            pCore->Launch()->fnLaunch = [pCore, pState, fnExecutor]()
            {
                RunExternalExecutor(pState, fnExecutor);
            };
            return promise;
        }

        RunExternalExecutor(pState, fnExecutor);
        return promise;
    }

    /// @brief then：上一层**兑现**时执行 fnHandler，被拒绝时直接透传（失败即停）。
    ///
    /// 尚未起链时，本调用即首层（起点结果视为已兑现）。上游未 settle 时登记
    /// （settle 时由 `RunHandler` 按线程亲和执行：同执行器内联 / 跨执行器投递回本链执行器）；
    /// 已 settle 时投递到执行器异步触发。
    /// 同一层多次 Then 即分叉，各自独立延续。
    ///
    /// @param fnHandler 本层处理器（固定签名）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄（后续 Await / Then / Catch / Finally 作用于本层）。
    CPromise Then(const ThenHandler& fnHandler, const CSourceLoc& loc = CSourceLoc())
    {
        return Append(fnHandler, loc, detail::kModeThen);
    }

    /// @brief then（**就地**版）：本层在「结算它的那条线程」上执行（不投递、不要求线程亲和）。
    ///
    /// 用途：显式覆盖默认线程亲和 —— 典型是跨模块返回后想直接跑在被调模块线程上
    /// （只做与对方相关的轻活时，可省一次回本模块的投递）。
    ///
    /// 注意：
    ///  - 本层可能跑在**别的模块的线程**上 → 不要碰本模块的非线程安全状态；
    ///  - 内联深度超过 `kMaxInlineDepth` 时仍会改投递回本链执行器（防爆栈）；
    ///  - 作首层时与其他层一样必须投递（起链线程不跑业务代码）。
    ///
    /// @param fnHandler 本层处理器（固定签名）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄。
    CPromise ThenInline(const ThenHandler& fnHandler, const CSourceLoc& loc = CSourceLoc())
    {
        return Append(fnHandler, loc, detail::kModeThen, detail::kAffinityInline);
    }

    /// @brief then（**指定执行器**版）：本层在给定执行器线程上执行（已在该线程则就地，否则投递）。
    ///
    /// 用途：把一个层放到本模块的另一个执行器（或测试里的专用执行器）上跑。
    ///
    /// @warning 执行器须存活到本层执行完毕（句柄保活，但被持对象不得提前析构）；
    ///          **不要用它把执行器跨模块传递**（执行器是模块私有资源，跨模块传是反模式）。
    ///
    /// @param executor 目标执行器。
    /// @param fnHandler 本层处理器（固定签名）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄。
    CPromise ThenOn(CAsyncExecutor& executor, const ThenHandler& fnHandler, const CSourceLoc& loc = CSourceLoc())
    {
        return Append(fnHandler, loc, detail::kModeThen, detail::kAffinityExecutor, executor.Handle());
    }

    /// @brief catch：上一层**被拒绝**时执行 fnHandler（回滚 / 补偿 / 错误处理）。
    ///
    /// 返回 `CPromiseResult::Resolve()` 即吞掉拒绝，链从本层之后继续；
    /// 返回 `upResult`（或任意 Reject）则继续以拒绝状态向下透传。
    /// 上一层已兑现时本层不执行，结果原样透传。
    ///
    /// @param fnHandler 本层处理器（固定签名，upResult 为上一层的拒绝结果）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄。
    CPromise Catch(const ThenHandler& fnHandler, const CSourceLoc& loc = CSourceLoc())
    {
        return Append(fnHandler, loc, detail::kModeCatch);
    }

    /// @brief finally：无论上一层兑现还是被拒绝都执行 fnHandler（收尾：清理 / 审计）。
    ///
    /// 与 JS 的 `finally` 一致：**忽略处理器返回的成败，原样透传上一层结果**
    /// （只有抛异常才会改变结果 → 本层被拒绝 kException）。
    /// 需要在失败时改变链的走向请用 Catch。
    ///
    /// @param fnHandler 本层处理器（固定签名，upResult 为上一层结果）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄。
    CPromise Finally(const ThenHandler& fnHandler, const CSourceLoc& loc = CSourceLoc())
    {
        return Append(fnHandler, loc, detail::kModeFinally);
    }

    /// @brief then 的 promise 版本（对齐 JS：处理器返回 promise 时链会等它 —— flatten）。
    ///
    /// 上一层**兑现**后执行 fnFactory 拿到一条子 promise，本层等它 settled：
    ///  - 子 promise 兑现 → 本层兑现；
    ///  - 子 promise 被拒绝 → 本层以同一拒绝码被拒绝（后续 Then 不执行，Catch / Finally 仍执行）；
    ///  - 上层被拒绝 → 本层不执行，拒绝原因原样透传（与 Then 一致）。
    ///
    /// 全程只登记回调、不占工作线程，**不阻塞**（单线程执行器也安全）——
    /// 这是「纯异步下调用其他模块 / 另一套上下文的异步函数」的标准写法：
    /// 子 promise 由 `CPromise::New` 桥接而来（见文件头「嵌套用法⑤」）。
    /// 尚未起链时本调用即首层（起点结果视为已兑现）。
    ///
    /// @param fnFactory 子 promise 工厂（入参为本流程共享上下文）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄。
    CPromise ThenPromise(const PromiseFactory& fnFactory, const CSourceLoc& loc = CSourceLoc())
    {
        if (m_pCore == nullptr)
        {
            return CPromise();  // 无效 promise：不注册任何处理器。
        }

        const std::shared_ptr<detail::CPromiseCore<TContext> > pCore = m_pCore;

        // ① 尚未起链：本层即首层（起点结果视为已兑现），投递执行（不在起链线程上跑）。
        if (m_pState == nullptr)
        {
            m_pState = std::make_shared<detail::CPromiseState>();
            m_pState->SetLoc(loc);
            const std::shared_ptr<detail::CPromiseState> pState = m_pState;
            std::function<void()> fnAdopt = [pCore, pState, fnFactory]()
            {
                Adopt(pCore, pState, fnFactory);
            };

            // 延迟启动链（BuildPromise）：只登记，等 Start() 再投递。
            if (pCore->Launch()->bDeferred)
            {
                pCore->Launch()->pFirst = pState;
                pCore->Launch()->fnLaunch = fnAdopt;
                return *this;
            }

            if (!detail::PostToHandle(pCore->Handle(), std::move(fnAdopt)))
            {
                pState->Settle(CPromiseResult::Reject(kStopped));  // 执行器不可用。
            }
            return *this;
        }

        // ② 已起链：追加一层，等子 promise settle 后收口本层。
        CPromise promiseNext;
        promiseNext.m_pCore = m_pCore;
        promiseNext.m_pState = std::make_shared<detail::CPromiseState>();
        const std::shared_ptr<detail::CPromiseState> pNextState = promiseNext.m_pState;
        pNextState->SetLoc(loc);

        const std::shared_ptr<detail::CPromiseState> pUpState = m_pState;
        const bool bOk =
            pUpState->AddHandler(pCore->Handle(), [pCore, pNextState, fnFactory](const CPromiseResult& upResult)
        {
            if (upResult.IsRejected())
            {
                pNextState->Settle(upResult);  // 上层被拒绝：失败即停（与 Then 一致）。
                return;
            }
            Adopt(pCore, pNextState, fnFactory);
        });

        if (!bOk)
        {
            // 上一层已 settled 但执行器不可用：本层无法执行，以拒绝结束（下游继续透传）。
            pNextState->Settle(CPromiseResult::Reject(kStopped));
        }
        return promiseNext;
    }

    /// @brief onSettled：本层 settled（兑现或拒绝）时触发一次收尾通知。
    ///
    /// 不产生新层、不改变结果；等价「观察最终结果」。
    ///
    /// **保证送达**：即使本层的执行器已停止 / 拒绝投递（典型：被调模块已 Stop），
    /// 通知也会执行（改在调用线程上就地执行）。所以调用方**不需要**检查返回值；
    /// 手写桥接里漏检返回值也不会让本层永久 pending（框架保证不会因此死等）。
    ///
    /// @param fnSettled 收尾通知（入参为本层最终结果）。
    /// @return true 已登记 / 已投递 / 已就地送达；false 仅当本 promise 无效（未绑定执行器）。
    bool OnSettled(const SettledHandler& fnSettled) const
    {
        if (m_pCore == nullptr || m_pState == nullptr)
        {
            return false;  // 无效 promise：无法注册（唯一返回 false 的情形）。
        }

        m_pState->AddSettledHandler(m_pCore->Handle(), [fnSettled](const CPromiseResult& result)
        {
            if (fnSettled)
            {
                fnSettled(result);
            }
        });
        return true;
    }

    /// @brief await：阻塞等待本层结果（JS await 的阻塞版，不抛异常）。
    ///
    /// @warning 这是**阻塞**等待，会占住当前工作线程：在层内 / 协程内直接调用
    ///          Await() 会占住一个 worker，若线程池已无空闲 worker，被等待的 promise
    ///          就无人执行 → **死锁**（单线程执行器必然死锁）。
    ///          要在异步流程里等异步，请优先用：
    ///           - `ThenPromise` / `CPromise::New`（纯异步、非阻塞，推荐，不需要协程）；
    ///           - 协程的 CO_AWAIT / CO_AWAIT_ALL（非阻塞挂起）；
    ///           - 层内「起子 promise 后由 OnSettled 回调续跑」（非阻塞，回调驱动）；
    ///           - 层内「先并行起、后续层里再等」（此时子 promise 多已完成，几乎不阻塞）。
    ///
    /// @return 本层最终结果；无效 promise 返回被拒绝（kStopped）。
    CPromiseResult Await() const
    {
        if (m_pState == nullptr)
        {
            return CPromiseResult::Reject(kStopped);  // 无效 promise：无结果可等。
        }
        // 延迟链未启动 → 自动启动（兜底：漏写 Start() 也不会死等）。
        if (m_pCore != nullptr && m_pCore->Launch()->bDeferred && !m_pCore->Launch()->bStarted)
        {
            const_cast<CPromise*>(this)->Start();
        }
        return m_pState->Await();
    }

    /// @brief 本层是否已 settled（兑现或拒绝）。
    bool IsSettled() const
    {
        return m_pState != nullptr && m_pState->IsSettled();
    }

    /// @brief 共享上下文（懒创建，有效 promise 上恒非空）。
    ///
    /// 外部可先取上下文填初始数据，再起链；也可在任意层读写。
    std::shared_ptr<TContext> GetContext() const
    {
        if (m_pCore == nullptr)
        {
            return std::shared_ptr<TContext>();
        }
        return m_pCore->Context();
    }

    /// @brief 本层的注册点源码位置（调试用；发布构建恒为空）。
    CSourceLoc Loc() const
    {
        return m_pState != nullptr ? m_pState->Loc() : CSourceLoc();
    }

   private:
    /// @brief 内部：执行 promise 工厂并 adopt 子 promise（ThenPromise 的收口逻辑）。
    ///
    /// 子 promise settled 时把结果转交本层状态（只登记回调，不阻塞任何线程）。
    ///
    /// @param pCore 共享核心（上下文 + 执行器句柄）。
    /// @param pState 本层状态（子 promise settle 后收口）。
    /// @param fnFactory 子 promise 工厂。
    static void Adopt(const std::shared_ptr<detail::CPromiseCore<TContext> >& pCore,
                      const std::shared_ptr<detail::CPromiseState>& pState, const PromiseFactory& fnFactory)
    {
        CPromise promiseChild;
        try
        {
            if (fnFactory)
            {
                promiseChild = fnFactory(pCore->Context());
            }
        }
        catch (...)
        {
            pState->Settle(CPromiseResult::Reject(kException));  // 工厂内异常 → 本层被拒绝。
            return;
        }

        if (!promiseChild.IsValid())
        {
            pState->Settle(CPromiseResult::Reject(kStopped));  // 工厂没给出可等待的子 promise。
            return;
        }

        const bool bOk = promiseChild.OnSettled([pState](CPromiseResult childResult)
        {
            pState->Settle(childResult);
        });
        if (!bOk)
        {
            // 防御：OnSettled 已保证送达（仅无效 promise 返回 false），正常路径不会走到这里。
            pState->Settle(CPromiseResult::Reject(kStopped));
        }
    }

    /// @brief 内部：执行外部 settle 体（`CPromise::New` 的 executor），把 resolve / reject 交给它。
    ///
    /// @param pState 本层状态（executor 通过 resolve / reject 收口它）。
    /// @param fnExecutor 执行体。
    static void RunExternalExecutor(const std::shared_ptr<detail::CPromiseState>& pState,
                                    const PromiseExecutor& fnExecutor)
    {
        ResolveFn fnResolve = [pState]()
        {
            pState->Settle(CPromiseResult::Resolve());
        };
        RejectFn fnReject = [pState](int nCode)
        {
            pState->Settle(CPromiseResult::Reject(nCode));
        };
        try
        {
            if (fnExecutor)
            {
                fnExecutor(fnResolve, fnReject);
            }
            else
            {
                fnReject(kRejected);  // 未给执行体：本 promise 直接被拒绝。
            }
        }
        catch (...)
        {
            fnReject(kException);  // executor 内异常 → 本 promise 被拒绝（与层内异常一致）。
        }
    }

    /// @brief 内部：标记为「延迟启动」链（`CAsyncExecutor::BuildPromise` 用）。
    void MarkDeferred()
    {
        if (m_pCore != nullptr)
        {
            m_pCore->Launch()->bDeferred = true;
        }
    }

    /// @brief 内部：追加一层（Then / Catch / Finally / ThenInline / ThenOn 共用；尚未起链时本层即首层）。
    ///
    /// @param fnHandler 本层处理器。
    /// @param loc 注册点源码位置。
    /// @param nMode 处理器模式（detail::kModeThen / kModeCatch / kModeFinally）。
    /// @param nAffinity 执行线程偏好（默认本链执行器；kAffinityInline 就地；kAffinityExecutor 用 pTarget）。
    /// @param pTarget 指定执行器句柄（kAffinityExecutor 时有效）。
    /// @return 指向本层的 promise 句柄（无效 promise 返回无效句柄）。
    CPromise Append(const ThenHandler& fnHandler, const CSourceLoc& loc, int nMode,
                    int nAffinity = detail::kAffinityChain,
                    const std::shared_ptr<detail::CExecutorHandle>& pTarget = nullptr)
    {
        if (m_pCore == nullptr)
        {
            return CPromise();  // 无效 promise：不注册任何处理器。
        }

        // 本层实际的执行器：默认本链执行器；指定执行器版用调用方给的那个。
        const std::shared_ptr<detail::CExecutorHandle> pExec =
            (nAffinity == detail::kAffinityExecutor && pTarget != nullptr) ? pTarget : m_pCore->Handle();

        // ① 尚未起链：本次调用即首层（起点结果视为「已兑现」），投递执行。
        //    首层总是投递（起链线程不跑业务代码）；kAffinityExecutor 时投递到指定执行器。
        if (m_pState == nullptr)
        {
            m_pState = std::make_shared<detail::CPromiseState>();
            m_pState->SetLoc(loc);

            // 延迟启动链（BuildPromise）：只登记首层动作，等 Start() 再投递。
            if (m_pCore->Launch()->bDeferred)
            {
                const std::shared_ptr<detail::CPromiseState> pFirst = m_pState;
                m_pCore->Launch()->pFirst = pFirst;
                m_pCore->Launch()->pTarget = (nAffinity == detail::kAffinityExecutor) ? pTarget : nullptr;
                if (nMode == detail::kModeCatch)
                {
                    // catch 作为首层：起点已兑现，没有可处理的拒绝 → 启动即 settled。
                    m_pCore->Launch()->fnLaunch = [pFirst]()
                    {
                        pFirst->Settle(CPromiseResult::Resolve());
                    };
                }
                else
                {
                    m_pCore->Launch()->fnLaunch =
                        detail::MakeHandlerRunner(m_pCore, pFirst, fnHandler, CPromiseResult::Resolve(), nMode);
                }
                return *this;
            }

            if (nMode == detail::kModeCatch)
            {
                // catch 作为首层：起点已兑现，没有可处理的拒绝 → 直接 settled（原样透传）。
                m_pState->Settle(CPromiseResult::Resolve());
            }
            else
            {
                detail::PostHandler(m_pCore, m_pState, fnHandler, CPromiseResult::Resolve(), nMode, pExec);
            }
            return *this;
        }

        // ② 已起链：追加一层（与上一层共享核心：同一上下文 + 同一执行器）。
        CPromise promiseNext;
        promiseNext.m_pCore = m_pCore;
        promiseNext.m_pState = std::make_shared<detail::CPromiseState>();
        const std::shared_ptr<detail::CPromiseState> pNextState = promiseNext.m_pState;
        pNextState->SetLoc(loc);

        const std::shared_ptr<detail::CPromiseCore<TContext> > pCore = m_pCore;
        const std::shared_ptr<detail::CPromiseState> pUpState = m_pState;
        const std::shared_ptr<detail::CExecutorHandle> pTargetExec = pTarget;

        const bool bOk = pUpState->AddHandler(
            pExec, [pCore, pNextState, fnHandler, nMode, nAffinity, pTargetExec](const CPromiseResult& upResult)
        {
            // then：上一层被拒绝 → 失败即停（本层不执行，拒绝原因原样交给下一层）。
            if (nMode == detail::kModeThen && upResult.IsRejected())
            {
                pNextState->Settle(upResult);
                return;
            }
            // catch：上一层已兑现 → 无事可做，原样交给下一层。
            if (nMode == detail::kModeCatch && upResult.IsFulfilled())
            {
                pNextState->Settle(upResult);
                return;
            }

            // finally：无论成败都执行（但忽略返回值）；then / catch：执行本层处理器。
            detail::RunHandler(pCore, pNextState, fnHandler, upResult, nMode, nAffinity, pTargetExec);
        });

        if (!bOk)
        {
            // 上一层已 settled 但目标执行器不可用：本层无法执行，以拒绝结束（下游继续透传）。
            pNextState->Settle(CPromiseResult::Reject(kStopped));
        }
        return promiseNext;
    }

    /// @brief 内部：从共享核心与状态构造句柄（协程 AsPromise 用）。
    ///
    /// @param pCore promise 共享核心。
    /// @param pState 状态（本句柄指向的层）。
    /// @return 指向该层的 promise 句柄。
    static CPromise Make(const std::shared_ptr<detail::CPromiseCore<TContext> >& pCore,
                         const std::shared_ptr<detail::CPromiseState>& pState)
    {
        CPromise promise;
        promise.m_pCore = pCore;
        promise.m_pState = pState;
        return promise;
    }

    friend class CAsyncExecutor;        // NewPromise 起链。
    friend class CCoroutine<TContext>;  // 协程 AsPromise / 子 promise。

    std::shared_ptr<detail::CPromiseCore<TContext> > m_pCore;  ///< 共享核心（上下文 + 执行器）。
    std::shared_ptr<detail::CPromiseState> m_pState;           ///< 当前层对应的状态。
};

/// @brief 起链实现（执行器入口，等价 JS `new Promise(executor)`）。
///
/// @tparam TContext 上下文类型（由 spContext 推导）。
/// @param spContext 共享上下文（所有层共用）。
/// @param fnHandler 首层处理器（固定签名）。
/// @param loc 注册点源码位置（可选）。
/// @return 指向首层的 promise 句柄。
template <typename TContext>
CPromise<TContext> CAsyncExecutor::NewPromise(const std::shared_ptr<TContext>& spContext,
                                              typename CPromise<TContext>::ThenHandler fnHandler,
                                              const CSourceLoc& loc /* = CSourceLoc() */)
{
    return CPromise<TContext>(*this, spContext, fnHandler, loc);
}

/// @brief 建一条「延迟启动」的 promise 链（先挂完层，再 Start）。
///
/// @tparam TContext 上下文类型（由 spContext 推导）。
/// @param spContext 共享上下文（所有层共用）。
/// @return 未启动的链句柄；`Start()`（或首次 `Await()`）后首层才投递执行。
template <typename TContext>
CPromise<TContext> CAsyncExecutor::BuildPromise(const std::shared_ptr<TContext>& spContext)
{
    CPromise<TContext> promise(*this, spContext);
    promise.MarkDeferred();
    return promise;
}

}  // namespace async
}  // namespace common
