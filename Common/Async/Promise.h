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

#include "Assert.h"
#include "Async/AsyncExecutor.h"
#include "Async/Diagnostics.h"
#include "Async/PromiseResult.h"
#include "Async/PromiseTypes.h"
#include "Async/SourceLoc.h"
#include "Async/Trace.h"

// ====================================================================
// CPromise —— 异步 promise（**链语义**对齐 JS 的 Promise / async-await）
//
// 注意对齐的边界：只有「链语义」（then / catch / finally / flatten / all / race…）来自 JS；
// 「链在哪条线程上跑、谁来投递」在 JS 里由宿主事件循环隐式承担，本框架则必须显式 —— 那就是
// CAsyncExecutor（见 AsyncExecutor.h），因此起链入口是 `exec.NewPromise(...)` 而非构造函数。
//
// 本框架不支持在层与层之间传递任意值：每层只产出「已兑现 / 已拒绝」
// （CPromiseResult），数据统一放在共享上下文（std::shared_ptr<TContext>）。
//
// 因此处理器（handler）签名固定：
//
//     CPromiseResult handler(CPromiseResult upResult,              // 上一层结果
//                            const std::shared_ptr<TContext>& spCtx); // 共享上下文
//
// 语义对照（JS 的 Promise；链语义与之一致，可直接套用直觉）：
//
//   new Promise(executor)          →  auto p = exec.NewPromise(spCtx, StepA);   // 起链 + 首层（立即投递）
//   new Promise((resolve, reject))  →  exec.NewPromise(spCtx, fnStarter);      // 起链由外部 settle（回调式异步接进来）
//     => { … 回调里 resolve()/reject()… }
//   promise.then(onFulfilled)      →  p.Then(StepB);        // 兑现时执行，拒绝直接透传
//   promise.catch(onRejected)      →  p.Catch(StepRollback);// 拒绝时执行，Resolve() 即恢复
//   promise.finally(onFinally)     →  p.Finally(StepLog);   // 无论成败都执行，不改结果
//   await promise                  →  p.Await()             // 阻塞等待（返回 CPromiseResult）
//   promise 已 settle              →  p.IsSettled()
//   resolve() / reject(reason)     →  CPromiseResult::Resolve() / CPromiseResult::Reject(码)
//   fulfilled / rejected           →  result.IsFulfilled() / result.IsRejected()
//   then(onFulfilled 返回 promise)  →  p.ThenPromise(FnFactory);   // 等子 promise（flatten）
//   + 把子 promise 的数据搬回本上下文 →  p.ThenBridge(FnCreate, FnApply);  // 跨模块 / 跨上下文桥接（推荐）
//   Promise.all([a, b])            →  exec.WhenAll(spCtx, a, b);         // 全部兑现才继续（任一拒绝立即失败）
//   Promise.allSettled([a, b])     →  exec.WhenAllSettled(spCtx, a, b);  // 全部落定即继续（不看成败）
//   Promise.race([a, b])           →  exec.WhenRace(spCtx, a, b);         // 首个落定者定结果
//   Promise.any([a, b])            →  exec.WhenAny(spCtx, a, b);          // 首个兑现者定结果（全拒绝才失败）
//
// 对照的**边界**（重要）：上面每一行右边都比 JS 多了东西 —— `exec`（调度器）与 `spCtx`
// （共享上下文），而且 `exec.*` 所在的那几行左边根本没有对应的 JS 写法：
//
//   JS 把两件事藏在语言 / 宿主里：闭包捕获一切（≈ 共享上下文）、事件循环隐式调度（≈ 执行器）。
//   C++ 两样都没有：上下文必须作为参数传进来，**调度必须由一个显式对象承担** —— 这就是
//   `CAsyncExecutor` 存在的全部理由（它接管了 JS 微任务队列的角色）。所以：
//
//   - `exec.NewPromise` / `exec.WhenAll` 是「**执行器上的**起链入口」，不是 Promise 的
//     构造函数 / 静态方法（JS 是 `new Promise(...)` 与 `Promise.all(...)`）；
//   - `p.Await()`（阻塞等待）在 JS 里**没有对应物**（`await` 不占线程），它的代价与替代
//     写法见 async-usage.md；
//   - `OnSettledOn` / `AwaitFor` 是**指定执行器 / 超时** API，JS 同样没有
//     （单线程事件循环不需要它们）。
//
// 调度侧的完整对照（Executor / io_context / TaskScheduler 等）见 AsyncExecutor.h。
//
// 语义要点：
//  - then / catch / finally 都返回「指向新一层的 promise」（与 JS 一致，链式可读）；
//  - 失败即停：then 层在上一层被拒绝时不执行，拒绝原因沿链透传；
//  - catch 层可恢复：返回 Resolve() 即吞掉拒绝，链从本层之后继续；
//  - finally 层只做收尾（回滚 / 清理 / 日志），**忽略返回值、原样透传上层结果**；
//  - 层内异常 → 本层被拒绝（kException），不向调用方抛出；
//  - 首层投递一次；后续层都回**本链执行器**：已在该执行器线程上就地级联（超过 kMaxInlineDepth
//    改投递防爆栈），否则（跨执行器 / 跨模块返回）投递回本链执行器 —— 所以**每层都在本链执行器线程上**，
//    逐层线程归属不需要逐个去想（要「换执行器」请用「模块自持执行器 + 子链 / `ThenBridge`」）。
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
//      ① 用 `exec.NewPromise(spCtx, fnStarter)` 造一条「由外部 settle」的 promise
//         （起链回调里发起别的模块的调用，在其 OnSettled 回调里 resolve() / reject(码)）；
//      ② 用 `p.ThenPromise([&]{ return bridgePromise; })` 把它接进本流程（then 的 promise 版）。
//      ③ ①② 合一、不用写样板的简写：`p.ThenBridge(fnCreate, fnApply, ASYNC_LOC)` ——
//       fnCreate 在轮到本层时起子链，fnApply 在子链兑现时把它的上下文数据搬进本上下文。
//    本流程的最终结果 = 含跨模块子流程的完整结果，全程不阻塞任何线程。
//
// ⑥ 汇聚多条并行分支（分叉 → 合流）：`exec.WhenAll(spCtx, pA, pB)` —— 组合器在**执行器**上
//    （与 `exec.NewPromise` 同族的起链入口），另有 `exec.WhenAllSettled` / `WhenRace` / `WhenAny`
//    （对齐 JS 同名 API）；子 promise 可跨上下文类型，聚合结果落定后照常 Then / Catch / Finally 继续。
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
//     (void)upResult;                                  // then 层不看上游结果：
//                                                      // 上一层被拒绝时框架直接跳过本层（失败即停），
//                                                      // 要处理拒绝请用 Catch（async-usage.md §4）
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
// 起链语义（与 JS 的 `new Promise(executor)` 一致）：**起链即投递首层**；追加层只登记
// （上游未 settle 时登记、已 settle 时投递），首层跑起来后按序推进。
// 若确实需要「构链期间不跑业务代码」，用一次 `exec.Post(...)` 把整段构链放到执行器线程上
// 执行即可 —— 框架不提供「延迟启动」这种双形态（一种链只有一种启动语义）。
// @endcode
//
// 文件结构（便于定位）：
//   一、detail 基础设施：CPromiseState（层状态机，含处理器登记策略；trace 记录只在调试构建）
//       ShouldPassThrough・ResolveLayerResult（三态语义）/ MakeHandlerRunner（任务体构造）
//       / CPromiseCore（共享核心：上下文 + 执行器句柄，兼层调度入口）
//       注：`HandlerMode`（then / catch / finally）在 "Async/PromiseTypes.h"；
//       执行器侧设施（CExecutorHandle、ShouldInline / DispatchInlineOrPost、内联深度）
//       在 "Async/AsyncExecutor.h"；
//       异步调用链（在层里看「我处在哪条链上」）在 "Async/Trace.h" / Trace.cpp。
//   二、CPromise：对外句柄（构造 / 起链 / 层方法 / 结果与通知 / 内部实现）
//       不变式：句柄恒指向一个已存在的层（无「未挂首层」态 → 无相关空判）
//   三、模板方法定义（执行器入口）：NewPromise
//
// 注：组合器（`exec.WhenAll` / `WhenAllSettled` / `WhenRace` / `WhenAny`）属于**执行器**的能力，
// 其声明、实现与文档均在 "Async/AsyncExecutor.h"；`CPromise` 侧不提供成员形态。
// ====================================================================

namespace common {
namespace async {

template <typename TContext>
class CPromise;
template <typename TContext>
class CCoroutine;

//================ 一、detail 基础设施 ================

namespace detail {

/// @brief 诊断文案（集中一处：测试断言常量，而不是去匹配子串）。
constexpr const char* kDiagNoticeThrow = "OnSettled 通知里抛出了异常（已忽略；通知不是层，没有结果可落）";
constexpr const char* kDiagAwaitRisk =
    "Await(): 层内（或本链执行器线程上）阻塞等待未落定的层 → 极可能死锁；"
    "请改用 ThenPromise / ThenBridge / OnSettled 回调续跑 / 协程 CO_AWAIT，或用 AwaitFor(ms) 兜底";

/// @brief 执行 settled 通知（异常兜底：通知里抛异常只报告，不向外抛）。
///
/// 通知不是「层」：它没有结果可落，也没人在等它。所以异常只能吞掉 ——
/// 但绝不能放任它逃出（`CPromiseState::Settle` 在锁外直接调用处理器，线程池
/// worker 不捕获异常 → 一旦逃出就是 std::terminate，整个进程完蛋）。
///
/// @param fnSettled 通知处理器（可为空）。
/// @param result 本层最终结果。
inline void RunNotice(const SettledHandler& fnSettled, const CPromiseResult& result)
{
    if (!fnSettled)
    {
        return;
    }
    try
    {
        fnSettled(result);
    }
    catch (...)
    {
        ReportDiagnostic(kDiagNoticeThrow);
    }
}

/// @brief 在指定执行器上执行 settled 通知（`OnSettledOn` 用）——与 `RunNotice` 对称。
///
/// 已在该执行器线程 → 就地；否则投递过去；执行器不可用 → 就地送达
/// （与「保证送达」一致，绝不丢通知）；异常兜底交给 `RunNotice`。
///
/// @param pTarget 目标执行器句柄。
/// @param fnSettled 通知处理器（可为空）。
/// @param result 本层最终结果。
inline void RunNoticeOn(
    const std::shared_ptr<CExecutorHandle>& pTarget, const SettledHandler& fnSettled, const CPromiseResult& result)
{
    if (!fnSettled)
    {
        return;
    }

    std::function<void()> fnRun = [fnSettled, result]()
    {
        RunNotice(fnSettled, result);
    };
    if (IsInExecutorThread(pTarget) || !PostToHandle(pTarget, std::move(fnRun)))
    {
        RunNotice(fnSettled, result);  // 已在目标线程 / 目标执行器不可用 → 就地（送达保证）。
    }
}

class CPromiseState;  // 前置声明（层状态自己也不在 trace 里被引用，这里只为了让下面的注释好写）。

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
    /// 处理器在调用方（结算）线程上被触发；若它是「层处理器」，再由 `RunHandler` 派发：
    /// 已在本链执行器线程 → 就地执行；否则投递回本链执行器。
    ///
    /// @param result 本层最终结果（已兑现 / 已拒绝）。
    void Settle(const CPromiseResult& result)
    {
        Handler handlerInline;
        std::vector<Handler> vecHandlers;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bSettled.load(std::memory_order_relaxed))
            {
                return;  // 单向开关：只 settle 一次。
            }
            m_result = result;
            m_bSettled.store(true, std::memory_order_relaxed);  // 锁内写；relaxed 即可。
            handlerInline = std::move(m_handlerInline);         // 第一个（1:1 链的常态）。
            vecHandlers.swap(m_vecHandlers);                    // 分叉出来的其余（登记序）。
        }

        // 支持多线程等待同一 promise（并发 Await）：notify_all 唤醒所有等待者。
        m_cv.notify_all();

        if (handlerInline)
        {
            handlerInline(result);
        }
        for (size_t i = 0; i < vecHandlers.size(); ++i)
        {
            if (vecHandlers[i])
            {
                vecHandlers[i](result);
            }
        }
    }

    /// @brief 登记处理器（执行器不可用时按策略收口）。
    ///
    /// 两种送达策略：
    ///  - **层处理器**（then / catch / finally / thenPromise，`bGuaranteedDelivery == false`）：
    ///    执行器不可用时返回 `false`，由调用方以 `kStopped` 收口本层（“停了的执行器不再跑新层”）；
    ///  - **通知**（`OnSettled`，`bGuaranteedDelivery == true`）：**保证送达** —— 执行器不可用时
    ///    在调用线程上就地执行，绝不丢弃（否则手写桥接漏检返回值就会让本层永久 pending、
    ///    上层 `Await()` 死等）。就地执行不会递归加深：通知里通常只是 settle 本层，
    ///    而本层后续处理器走 `RunHandler`，执行器不可用时以 `kStopped` 收口，链会立即结束。
    ///
    /// @param pHandle 执行器句柄（已 settled 时投递用）。
    /// @param fnHandler 处理器（按值接收，登记时移动存储避免拷贝）。
    /// @param bGuaranteedDelivery 是否要求「送达保证」（通知用 true）。
    /// @return true 已登记 / 已投递 / 已就地送达；false 仅当层处理器已 settled 且执行器不可用。
    bool AddHandler(const std::shared_ptr<CExecutorHandle>& pHandle, Handler fnHandler, bool bGuaranteedDelivery = false)
    {
        bool bFireNow = false;
        CPromiseResult result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_bSettled.load(std::memory_order_relaxed))
            {
                // 第一个处理器就地存（1:1 链的常态，免一次 vector 分配）；分叉的才进 vector。
                if (!m_handlerInline)
                {
                    m_handlerInline = std::move(fnHandler);
                }
                else
                {
                    m_vecHandlers.push_back(std::move(fnHandler));
                }
                return true;  // pending：已登记，settle 时触发（在结算线程上）。
            }
            bFireNow = true;
            result = m_result;
        }

        if (!bFireNow || !fnHandler)
        {
            return true;  // 已 settled 但没有处理器可跑。
        }

        // 已 settled：优先投递到执行器异步执行（与 JS 一致），保持调用方不阻塞。
        // 注意：按值传参（拷贝）—— 投递失败时 fnRun 仍可用（送达保证要就地执行它）。
        std::function<void()> fnRun = [fnHandler, result]()
        {
            fnHandler(result);
        };
        if (PostToHandle(pHandle, fnRun))
        {
            return true;
        }

        if (!bGuaranteedDelivery)
        {
            return false;  // 层处理器：执行器不可用 → 交给调用方以 kStopped 收口。
        }

        {
            CInlineGuard guard;  // 通知就地送达（与其它内联路径共用深度计数，防极端嵌套）。
            fnRun();
        }
        return true;
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
        m_cv.wait(lock,
            [this]()
            {
                return m_bSettled.load(std::memory_order_relaxed);
            });
        return m_result;
    }

    /// @brief 阻塞等待本状态 settle，最多等 nTimeoutMs 毫秒。
    ///
    /// @param nTimeoutMs 超时毫秒数（< 0 = 无限等待，等价 `Await()`）。
    /// @return 本层最终结果；超时返回被拒绝（kStopped）。
    CPromiseResult AwaitFor(int nTimeoutMs)
    {
        if (nTimeoutMs < 0)
        {
            return Await();
        }
        if (m_bSettled.load(std::memory_order_relaxed))
        {
            return Await();  // 已落定：直接走原路径（含短自旋）。
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, std::chrono::milliseconds(nTimeoutMs),
                [this]()
                {
                    return m_bSettled.load(std::memory_order_relaxed);
                }))
        {
            return CPromiseResult::Reject(kStopped);  // 超时：不落定本层，只向调用方报「没等到」。
        }
        return m_result;
    }

    /// @brief 本状态是否已 settled（兑现或拒绝）。
    bool IsSettled() const
    {
        return m_bSettled.load(std::memory_order_relaxed);
    }

#if defined(ASYNC_DEBUG_TRACE)

    //================ 调用链 trace（**只在调试构建存在**） ================
    //
    // 发布构建下这段整段不参与编译（连同上面的 m_trace 成员）：trace 不是「空操作版本」，
    // 而是根本没有 —— 调用方要写 trace 相关代码，请自己用 #if defined(ASYNC_DEBUG_TRACE) 包住。

    /// @brief 设置本层的注册点源码位置。
    ///
    /// @param loc 源码位置（建议传 ASYNC_LOC）。
    void SetLoc(const CSourceLoc& loc)
    {
        m_trace.loc = loc;
    }

    /// @brief 记下本层在调用链里的位置：上游层 + 模式 + 它在哪条链上。
    ///
    /// 上游用**强引用**：层的状态是靠「上游的处理器闭包」保活的，闭包用完即毁 —— 用弱引用的话，
    /// 中间层跑完就被释放，链会被截断，而那正是排障最需要它的时候。因此这些链接
    /// **一次写入、之后只读**（注册时设一次，永不释放），代价就是「只要下游还活着，
    /// 上游就不会被释放」—— 调试构建下整条链随尾层句柄存活。
    ///
    /// @param pUpstream 上游层（本层被挂到它上面；空 = 首层 / 层外起的链根）。
    /// @param eMode 本层模式（then / catch / finally）。
    /// @param bChainRoot 本层是不是「它那条链」的链根（起链的两处为 true，追加层为 false）。
    /// @param nChainId 本层的链号（链根：新分配的号；追加层：传上游的链号）。
    void SetTraceLink(const std::shared_ptr<CPromiseState>& pUpstream, HandlerMode eMode, bool bChainRoot, unsigned nChainId)
    {
        m_trace.upstream = pUpstream;
        m_trace.eMode = eMode;
        m_trace.bChainRoot = bChainRoot;
        m_trace.bSubChain = bChainRoot && (pUpstream != nullptr);  // 链根且有父层 = 子链
        m_trace.nChainId = nChainId;
    }

    /// @brief 分配层号（创建层状态时调一次）。
    ///
    /// @param nLayerId 层号（`detail::NextLayerId()`）。
    void SetLayerId(unsigned nLayerId)
    {
        m_trace.nLayerId = nLayerId;
    }

    /// @brief 记下「本层跑在哪条线程上」（开跑时由帧写一次，之后只读）。
    ///
    /// @param tid 当前线程 id。
    void SetRunningThread(const std::thread::id& tid)
    {
        m_trace.tid = tid;
    }

    /// @brief 记下「本层跑在哪个执行器上」（开跑时由帧写一次，之后只读）。
    ///
    /// 链跨执行器就跨链（子链有自己的执行器），所以这是逐层属性；名字串在执行器构造时分配一次，
    /// 各层共享（层记录持强引用：执行器析构后已起的链还会跑完）。
    ///
    /// @param spExecName 执行器名（空 = 未命名）。
    void SetTraceExec(const std::shared_ptr<const std::string>& spExecName)
    {
        m_trace.spExecName = spExecName;
    }

    /// @brief 记下本层处理器的耗时（落定前写一次）。
    ///
    /// @param nMs 毫秒数。
    void SetSelfDurationMs(long long nMs)
    {
        m_trace.nSelfMs = nMs;
    }

    /// @brief 本层链号（追加层用它继承上游的链号）。
    ///
    /// @return 链号。
    unsigned ChainId() const
    {
        return m_trace.nChainId;
    }

    /// @brief 本层的 trace 记录（**拷贝**）。
    ///
    /// 遍历（`VisitLayerChain` / `CurrentLayer`）拿到它之后会在上面填「视图字段」
    /// （深度 / 当前层 / 年龄 / 结果），不会影响层状态里存的那份。
    ///
    /// @return 本层记录。
    CLayerInfo LayerInfo() const
    {
        return m_trace;
    }

    /// @brief 取本层落定结果（链上能直接看到上游是兑现还是拒绝）。
    ///
    /// 带锁读：与 `Settle` 的写入同步（trace 会从**别的线程**看已经落定的上游层）。
    ///
    /// @param out 落定结果（返回 true 时有效）。
    /// @return 已落定 → true。
    bool TryGetResult(CPromiseResult& out) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_bSettled.load(std::memory_order_relaxed))
        {
            return false;
        }
        out = m_result;
        return true;
    }

#endif  // defined(ASYNC_DEBUG_TRACE)

private:
    mutable std::mutex m_mutex;          ///< 保护结果与处理器列表（mutable：trace 的只读取结果要加锁）。
    std::condition_variable m_cv;        ///< 通知等待者。
    Handler m_handlerInline;             ///< 第一个处理器（1:1 链常态，免 vector 分配）。
    std::vector<Handler> m_vecHandlers;  ///< 第二个起（同层分叉）才用。
    std::atomic<bool> m_bSettled;        ///< 是否已 settled（自旋读用）。
    CPromiseResult m_result;             ///< 最终结果（settled 后有效）。
#if defined(ASYNC_DEBUG_TRACE)
    /// 本层的 trace 记录（注册点 / 上游 / 模式 / 层号 / 链号 / 线程 / 耗时）。
    /// 类型就是 `CLayerInfo`（既是存储记录也是遍历视图，只此一份，没有第二个结构）。
    CLayerInfo m_trace;
#endif
};

// 处理器模式 `HandlerMode`（then / catch / finally）已挪到 "Async/PromiseTypes.h"：
// 它是「层语义」的公共词汇，`Async/Trace.h` 也要用（放叶子头文件里避免循环依赖）。

/// @brief 本层处理器是否跳过（JS 三态语义：then 被拒 / catch 已兑现 → 直接透传上一层结果）。
///
/// @param eMode 处理器模式（then / catch / finally）。
/// @param upResult 上一层结果。
/// @return true = 本层跳过（把 upResult 原样交给下一层）。
inline bool ShouldPassThrough(HandlerMode eMode, const CPromiseResult& upResult)
{
    return (eMode == kModeThen && upResult.IsRejected()) || (eMode == kModeCatch && upResult.IsFulfilled());
}

/// @brief 本层对外的最终结果（JS 三态语义：finally 忽略处理器返回值，原样透传上一层结果）。
///
/// @param eMode 处理器模式。
/// @param upResult 上一层结果。
/// @param ownResult 本层处理器返回的结果。
/// @return then / catch 取 ownResult；finally 取 upResult。
inline CPromiseResult ResolveLayerResult(HandlerMode eMode, const CPromiseResult& upResult, const CPromiseResult& ownResult)
{
    return (eMode == kModeFinally) ? upResult : ownResult;
}

/// @brief 构造「执行本层处理器」的任务体。
///
/// 任务体只捕获**它真正需要的东西**：共享上下文（handler 的第二参数）与本层状态（写回结果）。
/// 这样在途任务不需要靠核心存活（因此核心无需 `enable_shared_from_this`），
/// 也让「保活链」短一截：任务跑完前，只有它自己用到的对象在。
///
/// @param spContext 共享上下文（调用方在构造任务时解析好，恒非空）。
/// @param pState 本层状态（执行结果写入它）。
/// @param fnHandler 处理器（固定签名）。
/// @param upResult 上一层结果。
/// @param eMode 处理器模式（then / catch / finally）。
/// @return 任务体（在工作线程上执行处理器并 settle 本层状态）。
template <typename TContext>
std::function<void()> MakeHandlerRunner(const std::shared_ptr<TContext>& spContext, const std::shared_ptr<CPromiseState>& pState,
    const ThenHandler<TContext>& fnHandler, const CPromiseResult& upResult, HandlerMode eMode)
{
    ASSERT(spContext != nullptr);  // 任务体把上下文按值捕获交给处理器：必须已经备好。
    return [spContext, pState, fnHandler, upResult, eMode]()
    {
#if defined(ASYNC_DEBUG_TRACE)
        // 记录「当前层」：处理器内部就能通过 Trace.h 看到自己处在哪条链上。
        // 帧活在本次调用的栈上（零分配，shared_ptr 只是一次引用计数）；内联级联会自然形成嵌套的帧栈。
        const CCurrentLayerFrame frame(pState);
#endif
        CPromiseResult result;
        try
        {
            const CPromiseResult ownResult = fnHandler(upResult, spContext);
            result = ResolveLayerResult(eMode, upResult, ownResult);  // finally 忽略 ownResult，原样透传。
        }
        catch (...)
        {
            result = CPromiseResult::Reject(kException);  // 处理器抛异常 → 拒绝（finally 抛异常同样覆盖）。
        }
#if defined(ASYNC_DEBUG_TRACE)
        pState->SetSelfDurationMs(frame.ElapsedMs());  // trace：本层耗时（落定前写一次）。
#endif
        pState->Settle(result);  // settle 本层 → 触发下一层（同执行器内联 / 跨执行器投递）。
    };
}

/// @brief promise 共享核心：共享上下文 + 执行器句柄。
///
/// 一条链的所有层共用同一个核心（同一上下文 + 同一执行器），句柄持有者彼此
/// 保活（执行器析构后链仍安全跑完）。
///
/// 「本层怎么跑」的**调度策略**（就地内联 / 投递、内联深度限额）归属执行器侧
/// （`detail::ShouldInline` / `detail::DispatchInlineOrPost`，在 AsyncExecutor.h）；
/// 这里只做两件事：**造任务体**（`MakeHandlerRunner`，层语义）与**失败收口**（`kStopped`）。
///
/// 注：首层不走这里 —— 「起链即强制投递」是 `CPromise::StartChain` 的一条直路
/// （没有调度选择，也就没有分派器）。
template <typename TContext>
class CPromiseCore
{
public:
    /// @brief 创建核心。
    ///
    /// @param pHandle 执行器句柄（可为空：协程构造时尚未绑定执行器，`Start` 时注入）。
    /// @param spContext 共享上下文（**必传**；调用方负责在建链前备好数据，框架不管它的生命周期）。
    CPromiseCore(const std::shared_ptr<CExecutorHandle>& pHandle, const std::shared_ptr<TContext>& spContext)
        : m_pHandle(pHandle), m_spContext(spContext)
    {
        // 上下文强制传入：没有它就无从「共享」——断言把这一契约钉在唯一入口上。
        ASSERT_MSG(spContext != nullptr, "共享上下文必须由调用方传入（框架不做懒创建）");
    }

    /// @brief 共享上下文（恒非空、构造后只读）。
    ///
    /// 上下文是**强制传入**的（没有懒创建 —— 懒创建要让上下文可默认构造、要给一个 "可能还没准备好" 的
    /// 时间窗加锁，而收益只是省掉调用方一行 `make_shared`）。因此热路径（每层都会取一次）
    /// 可以直接取用，既不加锁、也不拷贝 `shared_ptr`。
    ///
    /// @return 共享上下文（引用在核心存活期内有效；要留到别的线程请自行拷贝一份）。
    const std::shared_ptr<TContext>& Context() const
    {
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

    /// @brief 级联执行下一层（上一层刚 settle，当前在主调方线程上）。
    ///
    /// **只有当前线程已经是本链执行器的线程**时才就地内联（省一次入队 + 保序）；
    /// 否则一律投递回本链执行器（典型场景：被调模块 settle 本链的层，本层就回到本模块线程执行）。
    /// 内联深度也只在同一执行器线程内累加，跨模块不会涨栈。
    ///
    /// @param pState 本层状态。
    /// @param fnHandler 处理器。
    /// @param upResult 上一层结果。
    /// @param eMode 处理器模式。
    void RunHandler(const std::shared_ptr<CPromiseState>& pState, const ThenHandler<TContext>& fnHandler,
        const CPromiseResult& upResult, HandlerMode eMode) const
    {
        ASSERT(pState != nullptr);  // 内部调用：本层状态恒存在。

        std::function<void()> fnRun = MakeHandlerRunner(Context(), pState, fnHandler, upResult, eMode);

        // 派发策略（就地 / 投递 / 深度限额）在执行器侧；这里只管「造任务体 + 失败收口」。
        if (!DispatchInlineOrPost(Handle(), std::move(fnRun)))
        {
            pState->Settle(CPromiseResult::Reject(kStopped));  // 执行器不可用 → 本层被拒绝。
        }
    }

private:
    std::shared_ptr<CExecutorHandle> m_pHandle;  ///< 执行器句柄。
    std::shared_ptr<TContext> m_spContext;       ///< 共享上下文（构造时传入，之后只读）。
};

}  // namespace detail

//================ 二、CPromise：对外句柄 ================

/// @brief 异步 promise 句柄（浅句柄：拷贝共享同一条 promise 链的同一层）。
///
/// 一条 promise 链 = 共享核心（上下文 + 执行器）+ 一串状态（每层一个）。
/// 本类只是「指向某一层」的句柄：
///  - 起链只有**执行器上的**公开入口：`exec.NewPromise(spCtx, 首层处理器)`（立即投递首层）、
///    `exec.NewPromise(spCtx, fnStarter)`（由外部回调 settle）、`exec.CoStart<T>(spCtx)`（协程）；
///    本类**不提供**任何起链入口，只做「句柄 + 加层」；
///  - **没有默认构造、也没有「无效句柄」这种对象**：句柄只能由起链入口或链上的层方法产出，
///    而且**恒指向一个真实存在的层**（起链时首层就已建好并投递）—— 所以「忘起链就挂层」
///    「对空句柄 Await」在编译期就不成立，框架内部也没有「未挂层」这种中间态要判；
///  - `Then` / `Catch` / `Finally` 追加一层并返回指向新层的句柄（等价 JS 的
///    `then` / `catch` / `finally`）；
///  - `Await` / `OnSettled` / `IsSettled` 作用于句柄所指的那一层。
///
/// @tparam TContext 共享上下文类型（用户自定义的流程数据结构）。
template <typename TContext>
class CPromise
{
public:
    //================ Types ================

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
    using ChainStarter = std::function<void(const ResolveFn& fnResolve, const RejectFn& fnReject)>;

    /// promise 工厂（ThenPromise 用）：返回一条需要等待的子 promise。
    ///
    /// 契约：工厂**必须**给出可等待的子 promise（没有「返回空表示没有子链」这条路 ——
    /// 真需要条件分支，就在工厂里返回不同形状的链）。
    using PromiseFactory = std::function<CPromise(const std::shared_ptr<TContext>& spContext)>;

    //================ Layer ================

    /// @brief then：上一层**兑现**时执行 fnHandler，被拒绝时直接透传（失败即停）。
    ///
    /// 在句柄所指的层之后**追加一层**：上游未 settle 时登记（settle 时由 `RunHandler` 派发；
    /// 执行：同执行器内联 / 跨执行器投递回本链执行器）；已 settle 时投递到执行器异步触发。
    /// 同一层多次 Then 即分叉，各自独立延续。
    ///
    /// @param fnHandler 本层处理器（固定签名）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄（后续 Await / Then / Catch / Finally 作用于本层）。
    CPromise Then(const ThenHandler& fnHandler, const CSourceLoc& loc = CSourceLoc())
    {
        return Append(fnHandler, loc, detail::kModeThen);
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
    /// 子 promise 由 `exec.NewPromise(spCtx, fnStarter)` 桥接而来（见文件头「嵌套用法⑤」）。
    ///
    /// @param fnFactory 子 promise 工厂（入参为本流程共享上下文）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄。
    CPromise ThenPromise(const PromiseFactory& fnFactory, const CSourceLoc& loc = CSourceLoc())
    {
        const std::shared_ptr<detail::CPromiseCore<TContext> > pCore = m_pCore;
        const std::shared_ptr<detail::CPromiseState> pUpState = m_pState;
        const std::shared_ptr<detail::CPromiseState> pNextState = NewLayerState(loc);
#if defined(ASYNC_DEBUG_TRACE)
        pNextState->SetTraceLink(pUpState, detail::kModeThen, /* bChainRoot = */ false, pUpState->ChainId());
#endif

        const bool bOk = pUpState->AddHandler(pCore->Handle(),
            [pCore, pNextState, fnFactory](const CPromiseResult& upResult)
            {
                if (upResult.IsRejected())
                {
                    pNextState->Settle(upResult);  // 失败即停（与 Then 一致）。
                    return;
                }
                Adopt(pCore, pNextState, fnFactory);  // 兑现：起子链并等它（收口本层）。
            });

        if (!bOk)
        {
            // 上一层已 settled 但执行器不可用：本层无法执行，以拒绝结束（下游继续透传）。
            SettleStopped(pNextState);
        }
        return CPromise(pCore, pNextState);
    }

    /// @brief 桥接一层：轮到本层时用 fnCreate 起一条「别的上下文」的子链，等它落定后把数据搬回本上下文，
    ///        再继续本链（= 手写 `New` + `OnSettled` 桥接的简写版，样板由框架收口）。
    ///
    /// 这是「跨模块 / 跨上下文调用」的推荐写法，等价 `ThenPromise` + 「子链落定后搬数据」两件事合一：
    ///  - `fnCreate(spSelf)` 在**本链执行器线程**上执行（只做「起子链 + 登记回调」，不要做重活）；
    ///  - 子链被拒绝 → 本层以**同一拒绝码**被拒绝（后续 Then 不执行，Catch / Finally 仍执行）；
    ///  - 子链兑现 → 先 `fnApply(spSelf, spChildCtx)` 把数据搬进本上下文，再兑现本层；
    ///  - 上层被拒绝 → 本层不执行，拒绝原因原样透传（与 Then / ThenPromise 一致）；
    ///  - 全程只登记回调、不占工作线程（单线程执行器也安全）。
    ///
    /// @warning `fnApply` 在**子链的结算线程**（典型：被调模块的线程）上执行 —— 通知不迁移。
    ///          它只应做「把子上下文的数据搬进本上下文」，不要碰本模块的其他状态；
    ///          要回到本模块线程干活，请放到桥接之后的层里（那些层会回本链执行器）。
    ///
    /// @param fnCreate 子链工厂：入参为本流程共享上下文，返回要等待的子 promise（上下文类型任意）。
    /// @param fnApply 数据搬运：入参为本流程上下文与子链上下文（仅子链兑现时调用；不需要搬数据时传空 lambda）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄。
    template <class TFnCreate, class TFnApply>
    CPromise ThenBridge(TFnCreate fnCreate, TFnApply fnApply, const CSourceLoc& loc = CSourceLoc())
    {
        typedef decltype(std::declval<TFnCreate>()(std::declval<const std::shared_ptr<TContext>&>())) TChildPromise;
        const std::shared_ptr<detail::CPromiseCore<TContext> > pCore = m_pCore;

        PromiseFactory fnFactory = [fnCreate, fnApply, pCore, loc](const std::shared_ptr<TContext>& spSelf) -> CPromise
        {
            TChildPromise promiseChild = fnCreate(spSelf);  // 起子链（抛异常 → Adopt 兜底为 kException）

            ChainStarter fnStarter = [promiseChild, fnApply, spSelf](const ResolveFn& fnResolve, const RejectFn& fnReject)
            {
                BindChildSettle(promiseChild, fnApply, spSelf, fnResolve, fnReject);  // 规则只有一份。
            };
            // 父层 = 桥接层（工厂跑在它下面的作用域里）：这一层（以及它等到的子链）都能追回本链。
            return NewFromHandle(pCore->Handle(), spSelf, fnStarter, loc);
        };
        return ThenPromise(fnFactory, loc);
    }

    //================ Result ================

    /// @brief onSettled：本层 settled（兑现或拒绝）时触发一次收尾通知。
    ///
    /// 不产生新层、不改变结果；等价「观察最终结果」。
    ///
    /// **恒送达**：即使本层的执行器已停止 / 拒绝投递（典型：被调模块已 Stop），
    /// 通知也会执行（改在调用线程上就地执行）—— 所以**没有返回值可检查**：登记即生效，
    /// 不会丢、也不会让本层永久 pending。
    ///
    /// @param fnSettled 收尾通知（入参为本层最终结果）。
    void OnSettled(const SettledHandler& fnSettled) const
    {
        m_pState->AddHandler(
            m_pCore->Handle(),
            [fnSettled](const CPromiseResult& result)
            {
                detail::RunNotice(fnSettled, result);  // 通知里抛异常：只报告，不逃出（否则 terminate）。
            },
            /* bGuaranteedDelivery = */ true);
    }

    /// @brief onSettled（**指定执行器**版）：通知在给定执行器线程上触发（不在结算线程）。
    ///
    /// 与 `OnSettled` 的唯一差别：通知会**投递到目标执行器**（已在该线程则就地），
    /// 用于「收尾 / 审计 / 指标上报要碰本模块状态」的场合（模块状态只在模块线程上改）。
    ///
    /// 送达保证与 `OnSettled` 一致：执行器不可用（已停止 / 拒绝投递）时在**结算线程**上就地执行，
    /// 绝不丢弃。异常同样只报告、不外抛。
    ///
    /// @warning 目标执行器须存活到通知送达（句柄保活，但被持对象不得提前析构）；
    ///          与其他「指定执行器」API 同理，**不要用它把执行器跨模块传递**。
    ///
    /// @param executor 目标执行器（典型：本模块的执行器）。
    /// @param fnSettled 收尾通知（入参为本层最终结果）。
    void OnSettledOn(CAsyncExecutor& executor, const SettledHandler& fnSettled) const
    {
        const std::shared_ptr<detail::CExecutorHandle> pTarget = executor.Handle();
        m_pState->AddHandler(
            pTarget,
            [pTarget, fnSettled](const CPromiseResult& result)
            {
                // 与 OnSettled 对称：通知路径各一行，差异只在「去哪条线程」。
                detail::RunNoticeOn(pTarget, fnSettled, result);
            },
            /* bGuaranteedDelivery = */ true);
    }

    /// @brief await：阻塞等待本层结果（JS await 的阻塞版，不抛异常）。
    ///
    /// @warning 这是**阻塞**等待，会占住当前工作线程：在层内 / 协程内直接调用
    ///          Await() 会占住一个 worker，若线程池已无空闲 worker，被等待的 promise
    ///          就无人执行 → **死锁**（单线程执行器必然死锁）。
    ///          要在异步流程里等异步，请优先用：
    ///           - `ThenPromise` / `exec.NewPromise(spCtx, fnStarter)`（纯异步、非阻塞，推荐，不需要协程）；
    ///           - 协程的 CO_AWAIT / CO_AWAIT_ALL（非阻塞挂起）；
    ///           - 层内「起子 promise 后由 OnSettled 回调续跑」（非阻塞，回调驱动）；
    ///           - 层内「先并行起、后续层里再等」（此时子 promise 多已完成，几乎不阻塞）。
    ///
    /// @return 本层最终结果。
    CPromiseResult Await() const
    {
        return WaitInternal(-1);  // < 0 = 无限等待。
    }

    /// @brief await（**带超时**）：最多等 nTimeoutMs 毫秒，超时不再阻塞。
    ///
    /// 用于**不允许永久挂住**的场合：测试、优雅关闭、启动自检。
    /// 超时只是向调用方报「没等到」（返回 `kStopped`），**不会取消或落定本层**——
    /// 链会继续在后台跑（要停链请用执行器 `Stop()` 或业务标记）。
    ///
    /// @warning 与 `Await()` 一样是阻塞等待；在层内 / 协程内调用同样会占住 worker
    ///          （在单线程执行器里可能死锁），异步流程里请优先用
    ///          `ThenPromise` / `ThenBridge` / `OnSettled` / 协程。
    ///
    /// @param nTimeoutMs 超时毫秒数（< 0 = 无限等待，等价 `Await()`）。
    /// @return 本层最终结果；超时返回被拒绝（kStopped）。
    CPromiseResult AwaitFor(int nTimeoutMs) const
    {
        return WaitInternal(nTimeoutMs);
    }

    /// @brief 本层是否已 settled（兑现或拒绝）。
    bool IsSettled() const
    {
        return m_pState->IsSettled();
    }

    /// @brief 共享上下文（恒非空：上下文由调用方强制传入）。
    ///
    /// 外部应先**备好数据再起链**（写法：先 `make_shared` 填初始数据，再 `exec.NewPromise(spCtx, …)`）；
    /// 也可在任意层读写。
    std::shared_ptr<TContext> GetContext() const
    {
        return m_pCore->Context();
    }

    /// @brief 本层的注册点源码位置（**只在调试构建存在**：发布构建没有 trace）。
    ///
    /// @return 注册点（`ASYNC_LOC` 传入的位置）。
#if defined(ASYNC_DEBUG_TRACE)
    CSourceLoc Loc() const
    {
        return m_pState->LayerInfo().loc;
    }
#endif

private:
    //================ Internal ================

    /// @brief 创建指向「某一层」的句柄（起链 / 层方法 / 协程 AsPromise 共用）。
    ///
    /// 私有不对外：句柄只能由「起链」或「链上的层方法」产出 —— 因此**句柄恒指向一个真实存在的层**，
    /// 框架内部不存在「句柄已建好但还没挂层」这种中间态（那是已删除的「延迟启动」唯一的产物）。
    ///
    /// @param pCore 共享核心（上下文 + 执行器句柄；恒非空）。
    /// @param pState 本句柄所指的层状态（恒非空）。
    CPromise(const std::shared_ptr<detail::CPromiseCore<TContext> >& pCore, const std::shared_ptr<detail::CPromiseState>& pState)
        : m_pCore(pCore), m_pState(pState)
    {
        ASSERT(pCore != nullptr);   // 句柄恒有核心（无「无效句柄」态）。
        ASSERT(pState != nullptr);  // 句柄恒指向一个层（起链时首层就已建好）。
    }

    /// @brief 内部：**起链** —— 建首层状态并投递首层（`exec.NewPromise(spCtx, handler)` 与协程
    ///        `NewPromise()` 共用的唯一入口）。
    ///
    /// 「建链 + 首层」是一个**原子动作**：这里建好首层状态后立刻投递，所以调用方拿到的句柄必然
    /// 指向一个已经在推进（或已落定）的层，不会出现「句柄在手但一层都没跑」的形态。
    ///
    /// 首层固定**强制投递**（不内联）：起链线程不执行任何业务代码。
    /// 注意这与 JS 的 `new Promise(executor)` 不同 —— 那边的 executor 只做「发起 + 登记回调」，
    /// 本来就是轻活；本框架的首层处理器是业务代码。
    ///
    /// @param pCore 共享核心（上下文 + 执行器句柄；恒非空）。
    /// @param fnHandler 首层处理器。
    /// @param loc 注册点源码位置。
    /// @return 指向首层的句柄（pending；执行器不可用时已被拒绝为 kStopped）。
    static CPromise StartChain(
        const std::shared_ptr<detail::CPromiseCore<TContext> >& pCore, const ThenHandler& fnHandler, const CSourceLoc& loc)
    {
        const std::shared_ptr<detail::CPromiseState> pState = NewLayerState(loc);

#if defined(ASYNC_DEBUG_TRACE)
        // trace：新链的链根挂在「起链时正在跑的层」下面 —— 这就是「子链 → 父链」那条边。
        // 必须在**投递之前**写好：链根一旦跑起来就可能被读，之后就只读了。
        pState->SetTraceLink(detail::CurrentLayerState(), detail::kModeThen, /* bChainRoot = */ true, detail::NextChainId());
#endif

        // 起点结果视为「已兑现」；首层恒以 then 语义执行（catch / finally 是追加层的写法）。
        std::function<void()> fnRun =
            detail::MakeHandlerRunner(pCore->Context(), pState, fnHandler, CPromiseResult::Resolve(), detail::kModeThen);
        if (!detail::PostToHandle(pCore->Handle(), std::move(fnRun)))
        {
            SettleStopped(pState);  // 执行器不可用 → 首层被拒绝（链绝不永久 pending）。
        }
        return CPromise(pCore, pState);
    }

    /// @brief 阻塞等待前的「死锁预警」（**不改变行为**，只报告，便于开发期定位）。
    ///
    /// 两种形态都报：
    ///  - **层内 / 通知内阻塞**（`InlineDepth() > 0`）：正卡在某个处理器里等异步 → 占住一个 worker，
    ///    没有空闲 worker 时被等待的层无人推进 → 死锁（单线程执行器必然）；
    ///  - **在本链执行器线程上等本链**：本链的后续层需要这条线程，而它正卡在这里 → 必然死锁
    ///    （典型误用：在工作线程上 `p.Await()` 等自己这条链）。
    ///
    /// 不做硬失败的原因：「本链执行器线程上等一个由别的线程 settle 的层」（例如桥接层）是能正常
    /// 返回的，硬失败会误伤。要避免永久挂住请用 `AwaitFor(ms)`。
    void ReportBlockingRisk() const
    {
        if (m_pState->IsSettled())
        {
            return;
        }
        if (detail::InlineDepth() > 0 || detail::IsInExecutorThread(m_pCore->Handle()))
        {
            ReportDiagnostic(detail::kDiagAwaitRisk);
        }
    }

    /// @brief 内部：建一层新状态（**层状态的唯一创建点**：起链的首层与追加的每一层都经此）。
    ///
    /// @param loc 注册点源码位置。
    /// @return 新层状态（pending）。
    static std::shared_ptr<detail::CPromiseState> NewLayerState(const CSourceLoc& loc)
    {
        const std::shared_ptr<detail::CPromiseState> pState = std::make_shared<detail::CPromiseState>();
#if defined(ASYNC_DEBUG_TRACE)
        pState->SetLoc(loc);
        pState->SetLayerId(detail::NextLayerId());  // trace：层号（创建即定，日志对账用）。
#else
        (void)loc;
#endif
        return pState;
    }

    /// @brief 内部：把「本层跑不了」收口为被拒绝（`kStopped`）——「层」唯一的失败收口点。
    ///
    /// 触发：上一层已 settled 但目标执行器不可用（被停 / 拒绝投递），或工厂没给出可等待的子链。
    /// 注意别当成 `kRejected`：语义不同。
    ///
    /// @param pState 本层状态。
    static void SettleStopped(const std::shared_ptr<detail::CPromiseState>& pState)
    {
        pState->Settle(CPromiseResult::Reject(kStopped));
    }

    /// @brief 内部：阻塞等待的统一入口（`Await` / `AwaitFor` 共用）：先预警，再交给本层状态。
    ///
    /// @param nTimeoutMs 超时毫秒数（< 0 = 无限等待）。
    /// @return 本层最终结果；超时返回被拒绝（kStopped）。
    CPromiseResult WaitInternal(int nTimeoutMs) const
    {
        ReportBlockingRisk();                   // 死锁预警（不改变行为，只报告）。
        return m_pState->AwaitFor(nTimeoutMs);  // AwaitFor 自行处理「< 0 = 无限等待」。
    }

    /// @brief 内部：把「子链落定 → 搬数据 → 收口」登记到子链上（`ThenBridge` 的唯一规则）。
    ///
    /// 子链被拒绝 → `fnReject(拒绝码)`；子链兑现 → `fnApply(本上下文, 子链上下文)` 后 `fnResolve()`；
    /// 搬运抛异常 → `fnReject(kException)`。子链上下文在本层线程上取好，避免搬运回调里再访子链。
    ///
    /// @param promiseChild 要等待的子链（上下文类型任意）。
    /// @param fnApply 数据搬运：入参为本流程上下文与子链上下文。
    /// @param spSelf 本流程共享上下文（交给 fnApply）。
    /// @param fnResolve 子链兑现后的收口动作。
    /// @param fnReject 失败收口动作（子链拒绝码 / 搬运异常的 kException）。
    template <class TChildContext, class TFnApply>
    static void BindChildSettle(const CPromise<TChildContext>& promiseChild, TFnApply fnApply,
        const std::shared_ptr<TContext>& spSelf, const ResolveFn& fnResolve, const RejectFn& fnReject)
    {
        const std::shared_ptr<TChildContext> spChildCtx = promiseChild.GetContext();  // 有效 promise 恒非空。
        promiseChild.OnSettled(
            [spChildCtx, fnApply, spSelf, fnResolve, fnReject](CPromiseResult childResult)
            {
                if (childResult.IsRejected())
                {
                    fnReject(childResult.Code());  // 子链拒绝：拒绝码原样透传。
                    return;
                }
                try
                {
                    fnApply(spSelf, spChildCtx);  // 搬数据（跑在子链结算线程上，见 ThenBridge 的 @warning）。
                }
                catch (...)
                {
                    fnReject(kException);  // 搬运抛异常 → 本层被拒绝（不向外抛）。
                    return;
                }
                fnResolve();
            });
    }

    /// @brief 内部：用执行器**句柄**创建「由外部兑现 / 拒绝」的 promise（`NewPromise` 的 ChainStarter 版与 `ThenBridge` 共用）。
    ///
    /// 拿的是句柄而不是执行器引用 —— 桥接层（`ThenBridge`）在工厂里要用「本链执行器」的句柄，
    /// 而那时已没有 `CAsyncExecutor&` 了。
    ///
    /// @param pHandle 执行器句柄（起链与续接投递用）。
    /// @param spContext 共享上下文（本 promise 所有层共用该实例）。
    /// @param fnStarter 起链回调（拿到 resolve / reject 句柄）。
    /// @param loc 注册点源码位置。
    /// @return 指向本 promise 的句柄（pending；由 fnStarter 触发 settle）。
    static CPromise NewFromHandle(const std::shared_ptr<detail::CExecutorHandle>& pHandle,
        const std::shared_ptr<TContext>& spContext, const ChainStarter& fnStarter, const CSourceLoc& loc)
    {
        // 待定：等外部 settle。
        const std::shared_ptr<detail::CPromiseState> pState = std::make_shared<detail::CPromiseState>();
#if defined(ASYNC_DEBUG_TRACE)
        pState->SetLoc(loc);
        pState->SetLayerId(detail::NextLayerId());  // trace：层号。
        // trace：链根挂在「起链时正在跑的层」下面（父层必须在投递 / 启动之前写好）。
        pState->SetTraceLink(detail::CurrentLayerState(), detail::kModeThen, /* bChainRoot = */ true, detail::NextChainId());
#else
        (void)loc;
#endif

        // 这里恒为「立即启动」：与 JS 的 `new Promise(executor)` 一样，起链回调当场同步执行。
        RunChainStarter(pState, fnStarter);
        return CPromise(std::make_shared<detail::CPromiseCore<TContext> >(pHandle, spContext), pState);
    }

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
        if (!fnFactory)
        {
            SettleStopped(pState);  // 没给工厂：本层无法产出子链。
            return;
        }

        try
        {
#if defined(ASYNC_DEBUG_TRACE)
            // trace：作用域内的起链都把新链根挂在「正在等子链的本层」下面
            // （工厂是在**上游层**的 settle 路径里跑的，不指定的话会落回上游层）。
            const detail::CChainAdopterScope scope(pState);
#endif
            const CPromise promiseChild = fnFactory(pCore->Context());
            // 通知恒送达（没有返回值）→ 子链落定即收口本层。
            promiseChild.OnSettled(
                [pState](CPromiseResult childResult)
                {
                    pState->Settle(childResult);
                });
        }
        catch (...)
        {
            pState->Settle(CPromiseResult::Reject(kException));  // 工厂内异常 / 子链构造失败 → 本层被拒绝。
        }
    }

    /// @brief 内部：把 resolve / reject 交给起链回调（`NewPromise(spCtx, starter)` 的起链回调）。
    ///
    /// @param pState 本层状态（起链回调通过 resolve / reject 收口它）。
    /// @param fnStarter 起链回调。
    static void RunChainStarter(const std::shared_ptr<detail::CPromiseState>& pState, const ChainStarter& fnStarter)
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
            if (fnStarter)
            {
                fnStarter(fnResolve, fnReject);
            }
            else
            {
                fnReject(kRejected);  // 未给执行体：本 promise 直接被拒绝。
            }
        }
        catch (...)
        {
            fnReject(kException);  // 起链回调内异常 → 本 promise 被拒绝（与层内异常一致）。
        }
    }

    /// @brief 内部：在当前层之后**追加一层**（Then / Catch / Finally / ThenPromise / ThenBridge 共用）。
    ///
    /// 追加 = 两件事：建新层状态 + 在当前层上登记「本层跑完后启动新层」的处理器。
    /// 当前层还没 settle 就只是登记（settle 时触发）；已 settle 则立即触发（`AddHandler` 内部投递）。
    ///
    /// @param fnHandler 本层处理器。
    /// @param loc 注册点源码位置。
    /// @param eMode 处理器模式（detail::kModeThen / kModeCatch / kModeFinally）。
    /// @return 指向新层的 promise 句柄。
    CPromise Append(const ThenHandler& fnHandler, const CSourceLoc& loc, detail::HandlerMode eMode)
    {
        const std::shared_ptr<detail::CPromiseCore<TContext> > pCore = m_pCore;
        const std::shared_ptr<detail::CPromiseState> pUpState = m_pState;
        const std::shared_ptr<detail::CPromiseState> pNextState = NewLayerState(loc);
#if defined(ASYNC_DEBUG_TRACE)
        pNextState->SetTraceLink(pUpState, eMode, /* bChainRoot = */ false, pUpState->ChainId());
#endif

        const bool bOk = pUpState->AddHandler(pCore->Handle(),
            [pCore, pNextState, fnHandler, eMode](const CPromiseResult& upResult)
            {
                // 三态语义：该跳过的层直接透传上一层结果（then 被拒 / catch 已兑现）。
                if (detail::ShouldPassThrough(eMode, upResult))
                {
                    pNextState->Settle(upResult);
                    return;
                }
                pCore->RunHandler(pNextState, fnHandler, upResult, eMode);
            });

        if (!bOk)
        {
            // 上一层已 settled 但目标执行器不可用：本层无法执行，以拒绝结束（下游继续透传）。
            SettleStopped(pNextState);
        }
        return CPromise(pCore, pNextState);
    }

    /// @brief 内部：从共享核心与状态构造句柄（协程 AsPromise 用）。
    ///
    /// @param pCore promise 共享核心。
    /// @param pState 状态（本句柄指向的层）。
    /// @return 指向该层的 promise 句柄。
    static CPromise Make(
        const std::shared_ptr<detail::CPromiseCore<TContext> >& pCore, const std::shared_ptr<detail::CPromiseState>& pState)
    {
        return CPromise(pCore, pState);
    }

    friend class CAsyncExecutor;        // NewPromise 起链。
    friend class CCoroutine<TContext>;  // 协程 AsPromise / 子 promise。

    std::shared_ptr<detail::CPromiseCore<TContext> > m_pCore;  ///< 共享核心（上下文 + 执行器；恒非空）。
    std::shared_ptr<detail::CPromiseState> m_pState;           ///< 本句柄所指的层状态（恒非空）。
};

//================ 三、模板方法定义（执行器入口） ================
//
// 这里放 `CAsyncExecutor` 模板成员的**定义**（声明与完整文档在 AsyncExecutor.h）：
//  - 起链：`NewPromise`（两个重载）。
//
// 为什么定义留在这里，而不是 AsyncExecutor.h：这两者都要**造 `CPromise` 实例**
// （用到注入点 `exec.NewPromise(spCtx, fnStarter)` 等内部构造路径），与 promise 机制放在一起读才完整。
// `CoStart`（定义在 Coroutine/Coroutine.h）同样遵循「声明在执行器头、实现跟着机制走」。
//
// 组合器（`WhenAll` 一族）不在此节：它们不碰 `CPromise` 的私有构造路径，
// 声明与实现都在 AsyncExecutor.h（该文件里对 `CPromise` 的使用全落在模板的依赖上下文）。

/// @brief 起链实现（执行器入口，等价 JS `new Promise(executor)`）。
///
/// @tparam TContext 上下文类型（由 spContext 推导）。
/// @param spContext 共享上下文（所有层共用）。
/// @param fnHandler 首层处理器（固定签名）。
/// @param loc 注册点源码位置（可选）。
/// @return 指向首层的 promise 句柄。
template <typename TContext>
CPromise<TContext> CAsyncExecutor::NewPromise(const std::shared_ptr<TContext>& spContext,
    typename CPromise<TContext>::ThenHandler fnHandler, const CSourceLoc& loc /* = CSourceLoc() */)
{
    // 起链 = 建核心（共享上下文 + 本执行器句柄）+ 起首层（建层状态 + 投递执行），只有这一条路。
    return CPromise<TContext>::StartChain(std::make_shared<detail::CPromiseCore<TContext> >(Handle(), spContext), fnHandler, loc);
}

/// @brief 起链实现（对齐 JS `new Promise(executor)`）：由 `fnStarter` 里的 resolve / reject 兑现。
///
/// 用途：把**其他模块 / 回调式**的异步接进本流程 —— 起链回调里发起调用并登记回调，
/// 由对方的完成回调调 `fnResolve()` 兑现或 `fnReject(码)` 拒绝（非阻塞，不占 worker）。
/// 与 JS 一致：起链回调 **立即（同步）执行**，因此只应做「发起 + 登记回调」，不要做重活。
///
/// @tparam TContext 上下文类型（由 spContext 推导）。
/// @param spContext 共享上下文（本 promise 所有层共用该实例）。
/// @param fnStarter 起链回调（对齐 JS executor：拿到 resolve / reject 句柄）。
/// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
/// @return 指向本 promise 的句柄（pending；由 fnStarter 触发 settle）。
template <typename TContext>
CPromise<TContext> CAsyncExecutor::NewPromise(const std::shared_ptr<TContext>& spContext,
    const typename CPromise<TContext>::ChainStarter& fnStarter, const CSourceLoc& loc /* = CSourceLoc() */)
{
    return CPromise<TContext>::NewFromHandle(Handle(), spContext, fnStarter, loc);
}

}  // namespace async
}  // namespace common
