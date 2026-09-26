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
#include "Async/GateGuard.h"
#include "Async/PromiseCore.h"
#include "Async/PromiseLayer.h"
#include "Async/PromiseResult.h"
#include "Async/PromiseState.h"
#include "Async/PromiseTypes.h"

// 注：本文件（promise 句柄 + 起链定义）依赖四块基础设施（原先都挤在本文件里，2026-09-26 拆开）：
//   - "Async/PromiseState.h"：一层一个状态机（单向开关）+ settled 通知路径；
//   - "Async/PromiseLayer.h"：层运行器（三态语义 + `Make*Runner`，含 `ASYNC_GATE` 挂起重入）；
//   - "Async/PromiseCore.h"：共享核心（上下文 + 执行器句柄 + 派发）；
//   - "Async/AsyncExecutor.h" / "Async/Trace.h"：调度层与调试构建的调用链。
// 本文件只保留「句柄（Then / Catch / Finally / Await / …）」与「起链（`NewPromise` 定义）」。
#include "Async/SourceLoc.h"
#include "Async/Trace.h"

// ====================================================================
// CPromise —— 异步 promise（「链语义」对齐 JS 的 Promise / async-await）
//
// 注意对齐的边界：只有「链语义」（then / catch / finally / flatten / all / race…）来自 JS；
// 「链在哪条线程上跑、谁来投递」在 JS 里由宿主事件循环隐式承担，本框架则必须显式 —— 那就是
// CAsyncExecutor（见 AsyncExecutor.h），因此起链入口是 `exec.NewPromise(...)` 而非构造函数。
//
// 本框架不支持在层与层之间传递任意值：每层只产出「已兑现 / 已拒绝」
// （CPromiseResult），数据统一放在共享上下文（std::shared_ptr<TContext>）。
//
// 因此处理器（handler）只有两种固定签名（then 一种，catch / finally 一种）：
//
//     CPromiseResult handler(const std::shared_ptr<TContext>& spCtx);              // then（含首层）
//     CPromiseResult handler(CPromiseResult upResult,                              // catch / finally
//                            const std::shared_ptr<TContext>& spCtx);
//
// 为什么要分两种：then 层能跑起来的前提就是「上游已兑现」（上游被拒绝时框架直接跳过本层），
// 所以上游结果对 then 层没有任何信息量（它拿不到也无需判断）；而 catch 要据此分支、
// finally 要原样透传，它们才需要那个形参。
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
//   resolve() / reject(reason)     →  CPromiseResult::Resolve() / CPromiseResult::Reject(异常)
//   拒绝的统一表达（含框架自己的失败） →  std::exception 派生类（结果里存 shared_ptr<const std::exception>；
//                                        业务细节放共享上下文，结果里没有错误码）
//   fulfilled / rejected           →  result.IsFulfilled() / result.IsRejected()
//   then(onFulfilled 返回 promise)  →  p.ThenPromise(FnFactory);   // 等子 promise（flatten）
//   + 把子 promise 的数据搬回本上下文 →  p.ThenBridge(FnCreate, FnApply);  // 跨模块 / 跨上下文桥接（推荐）
//   Promise.all([a, b])            →  exec.WhenAll(spCtx, a, b);         // 全部兑现才继续（任一拒绝立即失败）
//   Promise.allSettled([a, b])     →  exec.WhenAllSettled(spCtx, a, b);  // 全部落定即继续（不看成败）
//   Promise.race([a, b])           →  exec.WhenRace(spCtx, a, b);         // 首个落定者定结果
//   Promise.any([a, b])            →  exec.WhenAny(spCtx, a, b);          // 首个兑现者定结果（全拒绝才失败）
//
// 对照的「边界」（重要）：上面每一行右边都比 JS 多了东西 —— `exec`（调度器）与 `spCtx`
// （共享上下文），而且 `exec.*` 所在的那几行左边根本没有对应的 JS 写法：
//
//   JS 把两件事藏在语言 / 宿主里：闭包捕获一切（≈ 共享上下文）、事件循环隐式调度（≈ 执行器）。
//   C++ 两样都没有：上下文必须作为参数传进来，「调度必须由一个显式对象承担」 —— 这就是
//   `CAsyncExecutor` 存在的全部理由（它接管了 JS 微任务队列的角色）。所以：
//
//   - `exec.NewPromise` / `exec.WhenAll` 是「执行器上的起链入口」，不是 Promise 的
//     构造函数 / 静态方法（JS 是 `new Promise(...)` 与 `Promise.all(...)`）；
//   - `p.Await()`（阻塞等待）在 JS 里「没有对应物」（`await` 不占线程），它的代价与替代
//     写法见 async-usage.md；
//   - `OnSettledOn` / `AwaitFor` 是「指定执行器 / 超时」 API，JS 同样没有
//     （单线程事件循环不需要它们）。
//
// 调度侧的完整对照（Executor / io_context / TaskScheduler 等）见 AsyncExecutor.h。
//
// 语义要点：
//  - then / catch / finally 都返回「指向新一层的 promise」（与 JS 一致，链式可读）；
//  - 失败即停：then 层在上一层被拒绝时不执行，拒绝原因沿链透传；
//  - catch 层可恢复：返回 Resolve() 即吞掉拒绝，链从本层之后继续；
//  - finally 层只做收尾（回滚 / 清理 / 日志），「忽略返回值、原样透传上层结果」；
//  - 层内异常 → 本层以「系统侧失败」收口（`Reject(std::runtime_error(e.what()))`），不向调用方抛出；
//  - 首层投递一次；后续层都回「本链执行器」：已在该执行器线程上就地级联（超过 kMaxInlineDepth
//    改投递防爆栈），否则（跨执行器 / 跨模块返回）投递回本链执行器 —— 所以「每层都在本链执行器线程上」，
//    逐层线程归属不需要逐个去想（要「换执行器」请用「模块自持执行器 + 子链 / `ThenBridge`」）。
//
// 嵌套用法（异步里再起异步）：
//   ① 协程内 await（推荐，非阻塞挂起）：
//        CO_AWAIT(NewPromise(StepSub));          // 子 promise（同上下文）
//        CO_AWAIT(pChild->AsPromise());          // 子协程
//        协程还能 await 「别的上下文类型」的 promise（跨流程嵌套）：
//        CO_AWAIT(exec.NewPromise(spDbCtx, StepQuery, ASYNC_LOC));
//   ② 并行嵌套：CO_AWAIT_ALL(a, b, c)，其中 a/b/c 各自可以是多步子 promise（a.Then(...)）；
//   ③ 层内嵌套（非阻塞）：层里起子 promise，由它的 OnSettled 回调接着写上下文 / 起后续；
//   ④ 层内嵌套（阻塞）：层里 sub.Await() —— 会占住一个工作线程，「线程池必须还有空闲
//      worker」，否则死锁（单线程执行器必死），只适合子流程很短且并发余量充足的场合。
//
// ⑤ 跨模块 / 跨上下文组合（「纯异步、零阻塞、不需要协程」）：把别的 promise 桥接进本流程 ——
//      ① 用 `exec.NewPromise(spCtx, fnStarter)` 造一条「由外部 settle」的 promise
//         （起链回调里发起别的模块的调用，在其 OnSettled 回调里 resolve() / reject(码)）；
//      ② 用 `p.ThenPromise([&]{ return bridgePromise; })` 把它接进本流程（then 的 promise 版）。
//      ③ ①② 合一、不用写样板的简写：`p.ThenBridge(fnCreate, fnApply, ASYNC_LOC)` ——
//       fnCreate 在轮到本层时起子链，fnApply 在子链兑现时把它的上下文数据搬进本上下文。
//    本流程的最终结果 = 含跨模块子流程的完整结果，全程不阻塞任何线程。
//
// ⑥ 汇聚多条并行分支（分叉 → 合流）：`exec.WhenAll(spCtx, pA, pB)` —— 组合器在「执行器」上
//    （与 `exec.NewPromise` 同族的起链入口），另有 `exec.WhenAllSettled` / `WhenRace` / `WhenAny`
//    （对齐 JS 同名 API）；子 promise 可跨上下文类型，聚合结果落定后照常 Then / Catch / Finally 继续。
//
// 并发注意：并行/嵌套的子 promise 若共用同一份共享上下文，请让各分支只写「不同字段」
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
// CPromiseResult StepReadParam(const std::shared_ptr<CLoginContext>& spCtx)
// {
//     spCtx->strAccount = ReadAccountFromRequest();  // then 层拿不到（也无需看）上游结果；
//                                                    // 要处理拒绝请用 Catch（async-usage.md §4）
//     spCtx->strAccount = ReadAccountFromRequest();
//     return spCtx->strAccount.empty() ? CPromiseResult::Reject(std::runtime_error("账号为空"))
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
// 起链语义（与 JS 的 `new Promise(executor)` 一致）：「起链即投递首层」；追加层只登记
// （上游未 settle 时登记、已 settle 时投递），首层跑起来后按序推进。
// 若确实需要「构链期间不跑业务代码」，用一次 `exec.Post(类别, ...)` 把整段构链放到执行器线程上
// 执行即可 —— 框架不提供「延迟启动」这种双形态（一种链只有一种启动语义）。
// @endcode
//
// 文件结构（便于定位；2026-09-26 按职责拆成四个头，本文件是「句柄 + 起链」）：
//   一、detail 基础设施（其余三块各自成文件，本头 include 它们）：
//       "Async/PromiseState.h"  —— CPromiseState（层状态机：单向开关 + 处理器登记 + 通知路径）
//       "Async/PromiseLayer.h"  —— 三态语义 + MakeLayerRunner / MakeThenRunner / MakeResultRunner
//                                  （框架里**唯一**跑用户处理器的地方，含 `ASYNC_GATE` 挂起重入）
//       "Async/PromiseCore.h"   —— CPromiseCore（共享核心：上下文 + 执行器句柄 + 派发）
//       本文件内：kDiagAwaitRisk、NewNextLayer（追加层骨架）/ AppendThenLayer・AppendResultLayer
//                （三态语义 + 两种签名）
//       注：`HandlerMode`（then / catch / finally）在 "Async/PromiseTypes.h"；
//       执行器侧设施（CExecutorHandle、ShouldInline / DispatchInlineOrPost、内联深度）
//       在 "Async/AsyncExecutor.h"；
//       异步调用链（在层里看「我处在哪条链上」）在 "Async/Trace.h" / Trace.cpp。
//   二、CPromise：对外句柄（构造 / 起链 / 层方法 / 结果与通知 / 内部实现）
//       不变式：句柄恒指向一个已存在的层（无「未挂首层」态 → 无相关空判）
//   三、模板方法定义（执行器入口）：NewPromise
//
// 注：组合器（`exec.WhenAll` / `WhenAllSettled` / `WhenRace` / `WhenAny`）属于「执行器」的能力，
// 其声明在 "Async/AsyncExecutor.h"、定义在 "Async/Combine.h"（它们要造 `CPromise` 实例，
// 需要本类的完整类型 —— 故不能留在只前置声明本类的执行器头里）；`CPromise` 侧不提供成员形态。
// ====================================================================

namespace common {
namespace async {

template <typename TContext>
class CPromise;
template <typename TContext>
class CCoroutine;

//================ 一、detail 基础设施 ================

namespace detail {

/// @brief 诊断：`Await()` 的死锁预警（唯一消费点 = `CPromise::ReportBlockingRisk`）。
///
/// 注：通知的登记与送达（`CPromiseState::AddNotice` / `AddNoticeOn`，含「恒送达 + 异常不外抛」的包装）
/// 与层状态 `CPromiseState` 都在 "Async/PromiseState.h"（本头 include 它 —— 调用方的 include 不用改）。
constexpr const char* kDiagAwaitRisk =
    "Await(): 在层内 / 协程体内（或本执行器线程上）阻塞等待未落定的结果 → 极可能死锁；"
    "请改用 ThenPromise / ThenBridge / OnSettled 回调续跑 / CO_AWAIT，或用 AwaitFor(ms) 兜底";

// 「一层」的状态机（`CPromiseState`）与 settled 通知路径见 "Async/PromiseState.h"：
// 单向开关 pending → settled；锁内只做「发布结果 + 摘走处理器」，用户代码一律在锁外跑；
// 层处理器按本层类别过门投递，通知不过门但**恒送达**。

// 三态语义（then / catch / finally 谁跳过、谁透传）与三个「任务体构造器」见
// "Async/PromiseLayer.h"：`MakeLayerRunner`（框架里**唯一**跑用户处理器的地方，含 `ASYNC_GATE`
// 的挂起重入）、`MakeThenRunner`（then / 首层：只给上下文）、
// `MakeResultRunner`（catch / finally：给「上游结果 + 模式」，finally 靠它原样透传）。
//
// 共享核心（上下文 + 执行器句柄 + 派发）见 "Async/PromiseCore.h"。

}  // namespace detail

//================ 二、CPromise：对外句柄 ================

/// @brief 异步 promise 句柄（浅句柄：拷贝共享同一条 promise 链的同一层）。
///
/// 一条 promise 链 = 共享核心（上下文 + 执行器）+ 一串状态（每层一个）。
/// 本类只是「指向某一层」的句柄：
///  - 起链只有「执行器上的」公开入口：`exec.NewPromise(spCtx, 首层处理器)`（立即投递首层）、
///    `exec.NewPromise(spCtx, fnStarter)`（由外部回调 settle）、`exec.CoStart<T>(spCtx)`（协程）；
///    本类「不提供」任何起链入口，只做「句柄 + 加层」；
///  - 「没有默认构造、也没有「无效句柄」这种对象」：句柄只能由起链入口或链上的层方法产出，
///    而且「恒指向一个真实存在的层」（起链时首层就已建好并投递）—— 所以「忘起链就挂层」
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

    /// then 处理器类型（「不看上游结果」：共享上下文 → 本层结果）。
    using ThenHandler = detail::ThenHandler<TContext>;

    /// catch / finally 处理器类型（「要上游结果」：上一层结果 + 共享上下文 → 本层结果）。
    using ResultHandler = detail::ResultHandler<TContext>;

    /// 兑现函数（对齐 JS `new Promise` 交给 executor 的 resolve）。
    using ResolveFn = std::function<void()>;

    /// 拒绝函数（对齐 JS `new Promise` 交给 executor 的 reject）。
    ///
    /// 入参是本层的「整份拒绝结果」：业务失败用 `Reject(异常对象)`（异常里带自己的种类），
    /// 框架侧失败用 `执行器已停` / `等待超时` / `处理器异常` —— 文案与来源都不会在传递中丢掉。
    using RejectFn = std::function<void(CPromiseResult result)>;

    /// executor：对齐 JS `new Promise((resolve, reject) => { ... })` 的入参。
    ///
    /// 只应发起异步动作并注册回调，由回调调用 resolve() / reject(结果) 兑现或拒绝本
    /// promise —— 非阻塞，不占工作线程。
    using ChainStarter = std::function<void(const ResolveFn& fnResolve, const RejectFn& fnReject)>;

    /// promise 工厂（ThenPromise 用）：返回一条需要等待的子 promise。
    ///
    /// 契约：工厂「必须」给出可等待的子 promise（没有「返回空表示没有子链」这条路 ——
    /// 真需要条件分支，就在工厂里返回不同形状的链）。
    using PromiseFactory = std::function<CPromise(const std::shared_ptr<TContext>& spContext)>;

    //================ Layer ================

    /// @brief then：上一层「兑现」时执行 fnHandler，被拒绝时直接透传（失败即停）。
    ///
    /// 在句柄所指的层之后「追加一层」：上游未 settle 时登记（settle 时由 `RunThenHandler` 派发；
    /// 执行：同执行器内联 / 跨执行器投递回本链执行器）；已 settle 时投递到执行器异步触发。
    /// 同一层多次 Then 即分叉，各自独立延续。
    ///
    /// 「处理器看不到上游结果」（上一层被拒绝时本层根本不执行），所以它只接上下文：
    /// 要处理拒绝请用 `Catch`（那里才拿得到 `upResult`）。
    ///
    /// **类别两种写法**：
    ///  - 省略（缺省 `kDirect`）→ **不过读写门**：本层不占槽位、不与他人互斥；需要门保护时在函数体
    ///    第一行用 `ASYNC_GATE_READ()` / `ASYNC_GATE_WRITE()` 声明（「是读还是写」由函数自己说了算，
    ///    调用方不必操心；框架发现不在目标槽位会挂起本层并按声明类别过门重入）；
    ///  - 显式给 → 本层按该类别过门（读可并发 / 写独占）。
    ///
    /// @param fnHandler 本层处理器（then 签名）。
    /// @param eKind 本层类别（可选：缺省 `kDirect` = 不过门；显式给读 / 写即按该类别过门）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄（后续 Await / Then / Catch / Finally 作用于本层）。
    CPromise Then(const ThenHandler& fnHandler, TaskKind eKind = TaskKind::kDirect, const CSourceLoc& loc = CSourceLoc())
    {
        return AppendThenLayer(fnHandler, loc, eKind);
    }

    /// @brief catch：上一层「被拒绝」时执行 fnHandler（回滚 / 补偿 / 错误处理）。
    ///
    /// 返回 `CPromiseResult::Resolve()` 即吞掉拒绝，链从本层之后继续；
    /// 返回 `upResult`（或任意 Reject）则继续以拒绝状态向下透传。
    /// 上一层已兑现时本层不执行，结果原样透传。
    ///
    /// @param fnHandler 本层处理器（catch 签名：入参是本层要处理的失败结果）。
    /// @param eKind 本层类别（可选：缺省 `kDirect` = 不过门；回滚 / 补偿要碰状态 → `kWrite`，
    ///              或在函数体首行用 `ASYNC_GATE_WRITE()` 声明）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄。
    CPromise Catch(const ResultHandler& fnHandler, TaskKind eKind = TaskKind::kDirect, const CSourceLoc& loc = CSourceLoc())
    {
        return AppendResultLayer(fnHandler, detail::kModeCatch, loc, eKind);
    }

    /// @brief finally：无论上一层兑现还是被拒绝都执行 fnHandler（收尾：清理 / 审计）。
    ///
    /// 与 JS 的 `finally` 一致：「永不跳过」（与 `Catch` 的「已兑现就跳过」相对），
    /// 且「忽略处理器返回的成败，原样透传上一层结果」
    /// （只有抛异常才会改变结果 → 本层以 `处理器异常` / `e.what()` 收口）。
    /// 需要在失败时改变链的走向请用 Catch。
    ///
    /// @param fnHandler 本层处理器（finally 签名，`upResult` 为上一层结果）。
    /// @param eKind 本层类别（可选：缺省 `kDirect` = 不过门；收尾审计要碰状态 → `kWrite`，
    ///              或在函数体首行用 `ASYNC_GATE_WRITE()` 声明）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄。
    CPromise Finally(const ResultHandler& fnHandler, TaskKind eKind = TaskKind::kDirect, const CSourceLoc& loc = CSourceLoc())
    {
        return AppendResultLayer(fnHandler, detail::kModeFinally, loc, eKind);
    }

    /// @brief then 的 promise 版本（对齐 JS：处理器返回 promise 时链会等它 —— flatten）。
    ///
    /// 上一层「兑现」后执行 fnFactory 拿到一条子 promise，本层等它 settled：
    ///  - 子 promise 兑现 → 本层兑现；
    ///  - 子 promise 被拒绝 → 本层以同一拒绝码被拒绝（后续 Then 不执行，Catch / Finally 仍执行）；
    ///  - 上层被拒绝 → 本层不执行，拒绝原因原样透传（与 Then 一致）。
    ///
    /// 全程只登记回调、不占工作线程，「不阻塞」（单线程执行器也安全）——
    /// 这是「纯异步下调用其他模块 / 另一套上下文的异步函数」的标准写法：
    /// 子 promise 由 `exec.NewPromise(spCtx, fnStarter)` 桥接而来（见文件头「嵌套用法⑤」）。
    ///
    /// @param fnFactory 子 promise 工厂（入参为本流程共享上下文）。
    /// @param eKind 本层类别（**必填**：读可并发 / 写独占 / 直投不过门）——只决定本层过门；子链的类别由它自己的起链入口定。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄。
    CPromise ThenPromise(const PromiseFactory& fnFactory, TaskKind eKind, const CSourceLoc& loc = CSourceLoc())
    {
        const std::shared_ptr<detail::CPromiseCore<TContext> > pCore = m_pCore;
        const std::shared_ptr<detail::CPromiseState> pUpState = m_pState;

        // ① 建本层（trace 里挂在当前层的下面，模式仍算 then；类别 = 本层的）。
        const std::shared_ptr<detail::CPromiseState> pNextState = NewNextLayer(m_pState, loc, detail::kModeThen, eKind);

        // ② 在上游层登记「轮到本层时干什么」：上游失败 → 跳过；上游兑现 → 起子链并等它。
        //    注意 AddHandler 拿的是**新层**的类别（新层过门的方式由它自己的类别决定）。
        const bool bOk = pUpState->AddHandler(pCore->Handle(), pNextState->Kind(),
            [pCore, pNextState, fnFactory](const CPromiseResult& upResult)
            {
                // 上游失败 → 本层跳过，结果原样交给下一层（与 Then 一致）。
                if (upResult.IsRejected())
                {
                    pNextState->Settle(upResult);
                    return;
                }

                // 上游兑现 → 执行工厂拿子链，等它落定后收口本层。
                //    工厂会碰用户代码 → 与层体一样**按本层类别过门**（就地 / 投递）：
                //    上游由外部线程 settle 时，直接调就会在那个线程上、无槽位地跑（见 DispatchAction）。
                pCore->DispatchAction(pNextState,
                    [pCore, pNextState, fnFactory]()
                    {
                        Adopt(pCore, pNextState, fnFactory);
                    });
            });

        // ③ 上游早已落定、且执行器不可用（停了的执行器不再跑新层）→ 本层收口为框架侧失败。
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
    ///  - `fnCreate(spSelf)` 在「本链执行器线程」上执行（**按本层类别过门**：所以它碰模块状态是安全的，
    ///    但仍应只做「起子链 + 登记回调」这类轻活）；
    ///  - 子链被拒绝 → 本层以「同一拒绝码」被拒绝（后续 Then 不执行，Catch / Finally 仍执行）；
    ///  - 子链兑现 → 先 `fnApply(spSelf, spChildCtx)` 把数据搬进本上下文，再兑现本层；
    ///  - 上层被拒绝 → 本层不执行，拒绝原因原样透传（与 Then / ThenPromise 一致）；
    ///  - 全程只登记回调、不占工作线程（单线程执行器也安全）。
    ///
    /// @warning `fnApply` 在「子链的结算线程」（典型：被调模块的线程）上执行 —— 通知不迁移。
    ///          它只应做「把子上下文的数据搬进本上下文」，不要碰本模块的其他状态；
    ///          要回到本模块线程干活，请放到桥接之后的层里（那些层会回本链执行器）。
    ///
    /// @param fnCreate 子链工厂：入参为本流程共享上下文，返回要等待的子 promise（上下文类型任意）。
    /// @param fnApply 数据搬运：入参为本流程上下文与子链上下文（仅子链兑现时调用；不需要搬数据时传空 lambda）。
    /// @param eKind 本层类别（**必填**：读可并发 / 写独占 / 直投不过门）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向本层的 promise 句柄。
    template <class TFnCreate, class TFnApply>
    CPromise ThenBridge(TFnCreate fnCreate, TFnApply fnApply, TaskKind eKind, const CSourceLoc& loc = CSourceLoc())
    {
        typedef decltype(std::declval<TFnCreate>()(std::declval<const std::shared_ptr<TContext>&>())) TChildPromise;
        const std::shared_ptr<detail::CPromiseCore<TContext> > pCore = m_pCore;

        PromiseFactory fnFactory = [fnCreate, fnApply, pCore, loc](const std::shared_ptr<TContext>& spSelf) -> CPromise
        {
            TChildPromise promiseChild = fnCreate(spSelf);  // 起子链（抛异常 → Adopt 兜底为 Exception）

            ChainStarter fnStarter = [promiseChild, fnApply, spSelf](const ResolveFn& fnResolve, const RejectFn& fnReject)
            {
                BindChildSettle(promiseChild, fnApply, spSelf, fnResolve, fnReject);  // 规则只有一份。
            };
            // 父层 = 桥接层（工厂跑在它下面的作用域里）：这一层（以及它等到的子链）都能追回本链。
            // 这条「等子链」的内部链是**框架簿记层**：只被 settle、不跑业务代码 → 不过门（kDirect）。
            // 子链自己的类别由它的起链入口决定。
            return NewFromHandle(pCore->Handle(), spSelf, fnStarter, loc, detail::kKindBookkeeping);
        };
        return ThenPromise(fnFactory, eKind, loc);
    }

    //================ Result ================

    /// @brief onSettled：本层 settled（兑现或拒绝）时触发一次收尾通知。
    ///
    /// 不产生新层、不改变结果；等价「观察最终结果」。
    ///
    /// 「恒送达」：即使本层的执行器已停止 / 拒绝投递（典型：被调模块已 Stop），
    /// 通知也会执行（改在调用线程上就地执行）—— 所以「没有返回值可检查」：登记即生效，
    /// 不会丢、也不会让本层永久 pending。
    ///
    /// @param fnSettled 收尾通知（入参为本层最终结果）。
    void OnSettled(const SettledNotice& fnSettled) const
    {
        m_pState->AddNotice(m_pCore->Handle(), fnSettled);
    }

    /// @brief onSettled（「指定执行器」版）：通知在给定执行器线程上触发（不在结算线程）。
    ///
    /// 与 `OnSettled` 的唯一差别：通知会「投递到目标执行器」（已在该线程则就地），
    /// 用于「收尾 / 审计 / 指标上报要碰本模块状态」的场合（模块状态只在模块线程上改）。
    ///
    /// 送达保证与 `OnSettled` 一致：执行器不可用（已停止 / 拒绝投递）时在「结算线程」上就地执行，
    /// 绝不丢弃。异常同样只报告、不外抛。
    ///
    /// @warning 目标执行器须存活到通知送达（句柄保活，但被持对象不得提前析构）；
    ///          与其他「指定执行器」API 同理，「不要用它把执行器跨模块传递」。
    ///
    /// @param executor 目标执行器（典型：本模块的执行器）。
    /// @param fnSettled 收尾通知（入参为本层最终结果）。
    void OnSettledOn(CAsyncExecutor& executor, const SettledNotice& fnSettled) const
    {
        // 与 OnSettled 对称：差异只在「通知去哪条线程跑」——包装与兑异常都在登记入口里。
        m_pState->AddNoticeOn(executor.Handle(), fnSettled);
    }

    /// @brief await：阻塞等待本层结果（JS await 的阻塞版，不抛异常）。
    ///
    /// @warning 这是「阻塞」等待，会占住当前工作线程：在层内 / 协程内直接调用
    ///          Await() 会占住一个 worker，若线程池已无空闲 worker，被等待的 promise
    ///          就无人执行 → 「死锁」（单线程执行器必然死锁）。
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

    /// @brief await（「带超时」）：最多等 nTimeoutMs 毫秒，超时不再阻塞。
    ///
    /// 用于「不允许永久挂住」的场合：测试、优雅关闭、启动自检。
    /// 超时只是向调用方报「没等到」（返回框架侧拒绝「等待超时」），「不会取消或落定本层」——
    /// 链会继续在后台跑（要停链请用执行器 `Stop()` 或业务标记）。
    ///
    /// @warning 与 `Await()` 一样是阻塞等待；在层内 / 协程内调用同样会占住 worker
    ///          （在单线程执行器里可能死锁），异步流程里请优先用
    ///          `ThenPromise` / `ThenBridge` / `OnSettled` / 协程。
    ///
    /// @param nTimeoutMs 超时毫秒数（< 0 = 无限等待，等价 `Await()`）。
    /// @return 本层最终结果；超时返回框架侧拒绝「等待超时」（**本层未落定**）。
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
    /// 外部应先「备好数据再起链」（写法：先 `make_shared` 填初始数据，再 `exec.NewPromise(spCtx, …)`）；
    /// 也可在任意层读写。
    std::shared_ptr<TContext> GetContext() const
    {
        return m_pCore->Context();
    }

    /// @brief 本层的注册点源码位置（「只在调试构建存在」：发布构建没有 trace）。
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
    /// 私有不对外：句柄只能由「起链」或「链上的层方法」产出 —— 因此「句柄恒指向一个真实存在的层」，
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

    /// @brief 内部：「起链」 —— 建首层状态并投递首层（`exec.NewPromise(spCtx, handler)` 与协程
    ///        `NewPromise()` 共用的唯一入口）。
    ///
    /// 「建链 + 首层」是一个「原子动作」：这里建好首层状态后立刻投递，所以调用方拿到的句柄必然
    /// 指向一个已经在推进（或已落定）的层，不会出现「句柄在手但一层都没跑」的形态。
    ///
    /// 首层固定「强制投递」（不内联）：起链线程不执行任何业务代码。
    /// 注意这与 JS 的 `new Promise(executor)` 不同 —— 那边的 executor 只做「发起 + 登记回调」，
    /// 本来就是轻活；本框架的首层处理器是业务代码。
    ///
    /// @param pCore 共享核心（上下文 + 执行器句柄；恒非空）。
    /// @param eKind 首层类别（**必填**：读可并发 / 写独占 / 直投不过门）—— 只管首层；
    ///              后续每层各自在 `Then` 一族里给类别（核心不存类别：链上各层可以不同）。
    /// @param fnHandler 首层处理器。
    /// @param loc 注册点源码位置。
    /// @return 指向首层的句柄（pending；执行器不可用时已是框架侧拒绝「执行器已停」）。
    static CPromise StartChain(const std::shared_ptr<detail::CPromiseCore<TContext> >& pCore, TaskKind eKind,
        const ThenHandler& fnHandler, const CSourceLoc& loc)
    {
        // ① 建首层状态（层状态的唯一创建点；首层也是 then 语义，类别 = 起链时给的那个）。
        const std::shared_ptr<detail::CPromiseState> pState = NewLayerState(loc, eKind);

#if defined(ASYNC_DEBUG_TRACE)
        // ② trace：新链的链根挂在「起链时正在跑的层」下面 —— 这就是「子链 → 父链」那条边。
        // 必须在「投递之前」写好：链根一旦跑起来就可能被读，之后就只读了。
        pState->SetTraceLink(detail::CurrentLayerState(), detail::kModeThen, /* bChainRoot = */ true, detail::NextChainId());
#endif

        // 起点结果视为「已兑现」；首层恒以 then 语义执行（catch / finally 是追加层的写法）。
        // ③ 造首层任务体并「强制投递」（不内联：起链线程不跑业务代码）。
        std::function<void()> fnRun = detail::MakeThenRunner(pCore->Handle(), pCore->Context(), pState, fnHandler);
        if (!detail::PostToHandle(pCore->Handle(), eKind, std::move(fnRun)))
        {
            SettleStopped(pState);  // 执行器不可用 → 首层被拒绝（链绝不永久 pending）。
        }
        return CPromise(pCore, pState);
    }

    /// @brief 阻塞等待前的「死锁预警」（「不改变行为」，只报告，便于开发期定位）。
    ///
    /// 两种形态都报：
    ///  - 「层内 / 通知内阻塞」（`InlineDepth() > 0`）：正卡在某个处理器里等异步 → 占住一个 worker，
    ///    没有空闲 worker 时被等待的层无人推进 → 死锁（单线程执行器必然）；
    ///  - 「在本链执行器线程上等本链」：本链的后续层需要这条线程，而它正卡在这里 → 必然死锁
    ///    （典型误用：在工作线程上 `p.Await()` 等自己这条链）。
    ///
    /// 不做硬失败的原因：「本链执行器线程上等一个由别的线程 settle 的层」（例如桥接层）是能正常
    /// 返回的，硬失败会误伤。要避免永久挂住请用 `AwaitFor(ms)`。
    void ReportBlockingRisk() const
    {
        if (m_pState->IsSettled())
        {
            return;  // 已经落定：不会阻塞，无需预警。
        }

        // ① 层内 / 通知内阻塞：正卡在某个处理器里等异步 → 占住一个 worker；
        //    没有空闲 worker 时被等的层无人推进 → 死锁（单线程执行器必然）。
        // ② 在本链执行器线程上等本链：本链的后续层需要这条线程，而它正卡在这里 → 必然死锁。
        if (detail::InlineDepth() > 0 || detail::IsInExecutorThread(m_pCore->Handle()))
        {
            ReportDiagnostic(detail::kDiagAwaitRisk);
        }
    }

    /// @brief 内部：建一层新状态（「层状态的唯一创建点」：起链的首层与追加的每一层都经此）。
    ///
    /// @param loc 注册点源码位置。
    /// @param eKind 本层读写类别（读可并发 / 写独占 / 直投不过门）。
    /// @return 新层状态（pending）。
    static std::shared_ptr<detail::CPromiseState> NewLayerState(const CSourceLoc& loc, TaskKind eKind)
    {
        const std::shared_ptr<detail::CPromiseState> pState = std::make_shared<detail::CPromiseState>();
        pState->SetKind(eKind);  // 类别随层走：同一串层可以读 / 写混排。

#if defined(ASYNC_DEBUG_TRACE)
        pState->SetLoc(loc);
        pState->SetLayerId(detail::NextLayerId());  // trace：层号（创建即定，日志对账用）。
#else
        (void)loc;  // 注册点只服务 trace：发布构建没有 trace。
#endif
        return pState;
    }

    /// @brief 内部：把「本层跑不了」收口为「框架侧拒绝『执行器已停』」 ——「层」唯一的失败收口点。
    ///
    /// 触发：上一层已 settled 但目标执行器不可用（被停 / 拒绝投递）。
    /// 文案固定（「执行器已停」）；业务想区分自己的拒绝与框架失败时，比对 `Message()`。
    ///
    /// @param pState 本层状态。
    static void SettleStopped(const std::shared_ptr<detail::CPromiseState>& pState)
    {
        pState->Settle(CPromiseResult::Reject(std::runtime_error("执行器已停")));
    }

    /// @brief 内部：阻塞等待的统一入口（`Await` / `AwaitFor` 共用）：先预警，再交给本层状态。
    ///
    /// @param nTimeoutMs 超时毫秒数（< 0 = 无限等待）。
    /// @return 本层最终结果；超时返回框架侧拒绝「等待超时」。
    CPromiseResult WaitInternal(int nTimeoutMs) const
    {
        // 两步：① 先报死锁风险（只报告，不改变行为）；② 再交给本层状态等（超时由它自己处理）。
        ReportBlockingRisk();
        return m_pState->AwaitFor(nTimeoutMs);  // AwaitFor 自行处理「< 0 = 无限等待」。
    }

    /// @brief 内部：把「子链落定 → 搬数据 → 收口」登记到子链上（`ThenBridge` 的唯一规则）。
    ///
    /// 子链被拒绝 → `fnReject(整份结果)`（异常类型 / 文案都不丢）；子链兑现 →
    /// `fnApply(本上下文, 子链上下文)` 后 `fnResolve()`；搬运抛异常 → `fnReject(异常原样)`。
    /// 子链上下文在本层线程上取好，避免搬运回调里再访子链。
    ///
    /// @param promiseChild 要等待的子链（上下文类型任意）。
    /// @param fnApply 数据搬运：入参为本流程上下文与子链上下文。
    /// @param spSelf 本流程共享上下文（交给 fnApply）。
    /// @param fnResolve 子链兑现后的收口动作。
    /// @param fnReject 失败收口动作（子链的整份拒绝结果 / 搬运异常）。
    template <class TChildContext, class TFnApply>
    static void BindChildSettle(const CPromise<TChildContext>& promiseChild, TFnApply fnApply,
        const std::shared_ptr<TContext>& spSelf, const ResolveFn& fnResolve, const RejectFn& fnReject)
    {
        // ① 先把子链上下文取到手：搬运回调里就不必再访子链（子链可能已经跑完）。
        const std::shared_ptr<TChildContext> spChildCtx = promiseChild.GetContext();  // 有效 promise 恒非空。

        // ② 在子链上登记落定回调 —— 子链什么时候落定，本层就什么时候收口。
        promiseChild.OnSettled(
            [spChildCtx, fnApply, spSelf, fnResolve, fnReject](CPromiseResult childResult)
            {
                // ③ 子链失败：「整份结果」原样透传（异常类型 / 文案都不丢），本流程随即也失败。
                if (childResult.IsRejected())
                {
                    fnReject(childResult);
                    return;
                }

                // ④ 子链兑现：搬数据（这一步跑在「子链的结算线程」上，只应做搬运）。
                try
                {
                    fnApply(spSelf, spChildCtx);  // 搬数据（跑在子链结算线程上，见 ThenBridge 的 @warning）。
                }
                catch (const std::exception& e)
                {
                    fnReject(CPromiseResult::Reject(std::runtime_error(e.what())));  // 搬运抛异常 → 文本带走
                    return;
                }
                catch (...)
                {
                    fnReject(CPromiseResult::Reject(std::runtime_error("处理器异常")));  // 非 std 异常
                    return;
                }

                // ⑤ 搬运成功 → 兑现本层，链从桥接层之后继续。
                fnResolve();
            });
    }

    /// @brief 内部：用执行器「句柄」创建「由外部兑现 / 拒绝」的 promise
    ///        （`NewPromise` 的 ChainStarter 版与 `ThenBridge` 共用）。
    ///
    /// 拿的是句柄而不是执行器引用 —— 桥接层（`ThenBridge`）在工厂里要用「本链执行器」的句柄，
    /// 而那时已没有 `CAsyncExecutor&` 了。
    ///
    /// @param pHandle 执行器句柄（起链与续接投递用）。
    /// @param spContext 共享上下文（本 promise 所有层共用该实例）。
    /// @param fnStarter 起链回调（拿到 resolve / reject 句柄）。
    /// @param loc 注册点源码位置。
    /// @param eKind 本条（由外部 settle 的层）的类别；内部簿记链传 `kKindBookkeeping`（不过门）。
    /// @return 指向本 promise 的句柄（pending；由 fnStarter 触发 settle）。
    static CPromise NewFromHandle(const std::shared_ptr<detail::CExecutorHandle>& pHandle,
        const std::shared_ptr<TContext>& spContext, const ChainStarter& fnStarter, const CSourceLoc& loc, TaskKind eKind)
    {
        // ① 建「由外部 settle」的层状态（pending：等 fnStarter 里的 resolve / reject）。
        const std::shared_ptr<detail::CPromiseState> pState = NewLayerState(loc, eKind);

#if defined(ASYNC_DEBUG_TRACE)
        // trace：链根挂在「起链时正在跑的层」下面（父层必须在投递 / 启动之前写好）。
        pState->SetTraceLink(detail::CurrentLayerState(), detail::kModeThen, /* bChainRoot = */ true, detail::NextChainId());
#else
        (void)loc;  // 注册点只服务 trace：发布构建没有 trace。
#endif
        // ② 启动起链回调：与层体同一套「就地 / 过门」判定（`DispatchInlineOrPost`）。
        //    已在本门同类槽位里 → 就地同步跑（与 JS 的 `new Promise(executor)` 一致，零额外开销）；
        //    否则（门外线程 / 别的模块的门 / 换类别）→ 按 eKind 过门投递后再跑 ——
        //    「本层以什么身份进模块」在起链这一步也成立，跨模块调用不会把被调模块的状态晾在门外。
        if (!detail::DispatchInlineOrPost(pHandle, eKind,
                [pState, fnStarter]()
                {
                    RunChainStarter(pState, fnStarter);
                }))
        {
            // ③ 执行器不可用（已停止 / 门已关）：本层以「执行器已停」收口（与层派发同款）。
            SettleStopped(pState);
        }

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
        // ① 工厂是必须的（契约：必须给出可等待的子链）—— 没给就无从产出，直接收口本层。
        if (!fnFactory)
        {
            SettleStopped(pState);
            return;
        }

        try
        {
#if defined(ASYNC_DEBUG_TRACE)
            // trace：作用域内的起链都把新链根挂在「正在等子链的本层」下面
            // （工厂是在「上游层」的 settle 路径里跑的，不指定的话会落回上游层）。
            const detail::CChainAdopterScope scope(pState);
#endif
            // ② 执行工厂拿子链（作用域让「工厂里起的链」把链根挂到本层下面，trace 才追得回来）。
            const CPromise promiseChild = fnFactory(pCore->Context());

            // ③ 只登记回调、不等待：子链落定即 settle 本层（不占任何线程）。
            promiseChild.OnSettled(
                [pState](CPromiseResult childResult)
                {
                    pState->Settle(childResult);
                });
        }
        // ④ 工厂自己抛异常 / 子链构造失败 → 本层收口为失败（文本带走，不向调用方抛）。
        catch (const std::exception& e)
        {
            pState->Settle(CPromiseResult::Reject(std::runtime_error(e.what())));
        }
        catch (...)
        {
            pState->Settle(CPromiseResult::Reject(std::runtime_error("处理器异常")));  // 非 std 异常
        }
    }

    /// @brief 内部：把 resolve / reject 交给起链回调（`NewPromise(spCtx, starter)` 的起链回调）。
    ///
    /// @param pState 本层状态（起链回调通过 resolve / reject 收口它）。
    /// @param fnStarter 起链回调。
    static void RunChainStarter(const std::shared_ptr<detail::CPromiseState>& pState, const ChainStarter& fnStarter)
    {
        // ① 备好交给起链回调的两个句柄：它们只是把结果转交本层状态（Settle 幂等）。
        ResolveFn fnResolve = [pState]()
        {
            pState->Settle(CPromiseResult::Resolve());
        };
        RejectFn fnReject = [pState](CPromiseResult result)
        {
            // 起链回调给的整份结果：异常类型 + 文案，框架只搬运、不解释。
            pState->Settle(result);
        };
        // ② 同步执行起链回调（就地分支：已在本门同类槽位里，只应做「发起 + 登记回调」，不要做重活）。
        try
        {
            if (fnStarter)
            {
                fnStarter(fnResolve, fnReject);
            }
            else
            {
                // 未给执行体：本 promise 直接被拒绝（框架侧，无更具体原因）。
                pState->Settle(CPromiseResult::Reject(std::runtime_error("未指定原因")));
            }
        }
        catch (const std::exception& e)
        {
            // ③ 起链回调内异常 → 本 promise 被拒绝（与层内异常一致）；文本带走（类型降级为 runtime_error）。
            pState->Settle(CPromiseResult::Reject(std::runtime_error(e.what())));
        }
        catch (...)
        {
            pState->Settle(CPromiseResult::Reject(std::runtime_error("处理器异常")));  // 非 std 异常
        }
    }

    /// @brief 内部：建「下一层」状态并记好 trace 链接（追加层的第一步，两种追加共用）。
    ///
    /// @param pUpState 上游层状态（本层挂在它后面）。
    /// @param loc 注册点源码位置。
    /// @param eMode 处理器模式（detail::kModeThen / kModeCatch / kModeFinally；只服务 trace）。
    /// @param eKind 本层类别（调用方已解析好：每层各自生效）。
    /// @return 新层状态（pending）。
    static std::shared_ptr<detail::CPromiseState> NewNextLayer(
        const std::shared_ptr<detail::CPromiseState>& pUpState, const CSourceLoc& loc, detail::HandlerMode eMode, TaskKind eKind)
    {
        // ① 建新层状态（层号在这里分配；类别随层走）。
        const std::shared_ptr<detail::CPromiseState> pNextState = NewLayerState(loc, eKind);

#if defined(ASYNC_DEBUG_TRACE)
        // ② trace：记下「本层从哪一层挂上来的 + 什么模式」，层里排障时据此反查整条链。
        pNextState->SetTraceLink(pUpState, eMode, /* bChainRoot = */ false, pUpState->ChainId());
#else
        (void)pUpState;  // 上游与模式都只服务 trace：发布构建没有 trace。
        (void)eMode;
#endif
        return pNextState;
    }

    /// @brief 内部：在当前层之后「追加一层 then」（处理器看不到上游结果）。
    ///
    /// 追加 = 两件事：建新层状态 + 在当前层上登记「本层跑完后启动新层」的处理器。
    /// 当前层还没 settle 就只是登记（settle 时触发）；已 settle 则立即触发（`AddHandler` 内部投递）。
    ///
    /// 三态语义：「上游被拒绝 → 本层跳过」（结果原样交给下一层，处理器根本不会被调用）。
    /// 所以本层跑起来的前提已定，处理器交 `RunThenHandler` 即可 —— 它只接共享上下文。
    ///
    /// @param fnHandler 本层处理器（then 签名）。
    /// @param loc 注册点源码位置。
    /// @param eKind 本层类别（读可并发 / 写独占 / 直投不过门）。
    /// @return 指向新层的 promise 句柄。
    CPromise AppendThenLayer(const ThenHandler& fnHandler, const CSourceLoc& loc, TaskKind eKind)
    {
        const std::shared_ptr<detail::CPromiseCore<TContext> > pCore = m_pCore;

        // ① 建新层（挂在本层之后，模式 = then；类别 = 本层的）。
        const std::shared_ptr<detail::CPromiseState> pNextState = NewNextLayer(m_pState, loc, detail::kModeThen, eKind);

        // ② 在本层登记「本层跑完后启动新层」：本层未落定就只是登记；已落定则立刻投递去跑。
        //    注意 AddHandler 拿的是**新层**的类别：新层在本层 settle 后才跑，它决定新层过门的方式。
        const bool bOk = m_pState->AddHandler(pCore->Handle(), pNextState->Kind(),
            [pCore, pNextState, fnHandler](const CPromiseResult& upResult)
            {
                // 三态语义（then）：上游被拒绝 → 本层跳过，结果原样交给下一层（失败即停）。
                if (upResult.IsRejected())
                {
                    pNextState->Settle(upResult);
                    return;
                }

                // 上游已兑现才走到这里：造本层任务体并派发（就地级联 / 投递回本链执行器）。
                pCore->RunThenHandler(pNextState, fnHandler);
            });

        // ③ 本层已落定但执行器不可用 → 新层跑不了，收口为框架侧失败（下游继续透传）。
        if (!bOk)
        {
            // 上一层层已 settled 但目标执行器不可用：本层无法执行，以拒绝结束（下游继续透传）。
            SettleStopped(pNextState);
        }
        return CPromise(pCore, pNextState);
    }

    /// @brief 内部：在当前层之后「追加一层 catch / finally」（处理器拿到上游结果）。
    ///
    /// 与 `AppendThenLayer` 同骨架，只差两处：处理器要「上游结果」（交 `RunResultHandler`），
    /// 以及模式要带到处理器里（finally 靠它「忽略返回值、原样透传」）。
    ///
    /// 三态语义：
    ///  - catch：「上游已兑现 → 本层跳过」（拒绝才有得处理）；返回 `Resolve()` 即恢复链；
    ///  - finally：「永不跳过」（成败都执行），所以下面没有它的跳过分支 —— 它的 eMode 只在
    ///    `MakeResultRunner` 里用来忽略处理器返回值、原样透传上一层结果。
    ///
    /// @param fnHandler 本层处理器（catch / finally 签名）。
    /// @param eMode 处理器模式（detail::kModeCatch / kModeFinally）。
    /// @param loc 注册点源码位置。
    /// @param eKind 本层类别（读可并发 / 写独占 / 直投不过门）。
    /// @return 指向新层的 promise 句柄。
    CPromise AppendResultLayer(const ResultHandler& fnHandler, detail::HandlerMode eMode, const CSourceLoc& loc, TaskKind eKind)
    {
        const std::shared_ptr<detail::CPromiseCore<TContext> > pCore = m_pCore;
        // ① 建新层（挂在本层之后；模式带下去，finally 靠它忽略返回值；类别 = 本层的）。
        const std::shared_ptr<detail::CPromiseState> pNextState = NewNextLayer(m_pState, loc, eMode, eKind);

        // ② 在本层登记「本层跑完后启动新层」：未落定 → 只登记；已落定 → 立刻投递去跑。
        //    注意 AddHandler 拿的是**新层**的类别（新层过门的方式由它自己的类别决定）。
        const bool bOk = m_pState->AddHandler(pCore->Handle(), pNextState->Kind(),
            [pCore, pNextState, fnHandler, eMode](const CPromiseResult& upResult)
            {
                // 三态语义（catch）：上游已兑现 → 本层跳过（finally 从不跳过：成败都执行）。
                if (eMode == detail::kModeCatch && upResult.IsFulfilled())
                {
                    pNextState->Settle(upResult);
                    return;
                }

                // 该跑的层才走到这里：任务体带着「上游结果 + 模式」，交给派发器。
                pCore->RunResultHandler(pNextState, fnHandler, upResult, eMode);
            });

        // ③ 本层已落定但执行器不可用 → 新层跑不了，收口为框架侧失败（下游继续透传）。
        if (!bOk)
        {
            // 上一层层已 settled 但目标执行器不可用：本层无法执行，以拒绝结束（下游继续透传）。
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
// 这里放 `CAsyncExecutor` 模板成员的「定义」（声明与完整文档在 AsyncExecutor.h）：
//  - 起链：`NewPromise`（两个重载）。
//
// 为什么定义留在这里，而不是 AsyncExecutor.h：这两者都要「造 `CPromise` 实例」
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
/// @param eKind 首层类别（**必填**：读可并发 / 写独占 / 直投不过门）—— 本框架不给默认值；
///              后续层各自在 `Then` 一族里给类别。
/// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
/// @return 指向首层的 promise 句柄。
template <typename TContext>
CPromise<TContext> CAsyncExecutor::NewPromise(const std::shared_ptr<TContext>& spContext,
    typename CPromise<TContext>::ThenHandler fnHandler, TaskKind eKind, const CSourceLoc& loc /* = CSourceLoc() */)
{
    // 起链 = 建核心（共享上下文 + 本执行器句柄）+ 起首层（建层状态 + 投递执行），只有这一条路。
    return CPromise<TContext>::StartChain(
        std::make_shared<detail::CPromiseCore<TContext> >(Handle(), spContext), eKind, fnHandler, loc);
}

/// @brief 起链实现（对齐 JS `new Promise(executor)`）：由 `fnStarter` 里的 resolve / reject 兑现。
///
/// 用途：把「其他模块 / 回调式」的异步接进本流程 —— 起链回调里发起调用并登记回调，
/// 由对方的完成回调调 `fnResolve()` 兑现或 `fnReject(std::runtime_error("原因"))` 收口（非阻塞，不占 worker）。
///
/// 启动时机与层体**完全一致**（`DispatchInlineOrPost`）：已在本门同类槽位里 → 就地同步跑
/// （JS 的 `new Promise(executor)` 语义）；否则（门外线程 / 别的模块的门 / 换类别）→
/// 按 `eKind` 过门投递后再跑 —— 「本层以什么身份进模块」在起链这一步也成立，
/// 跨模块调用不会把被调模块的状态晾在门外。故起链回调只应做「发起 + 登记回调」，不要做重活。
///
/// @tparam TContext 上下文类型（由 spContext 推导）。
/// @param spContext 共享上下文（本 promise 所有层共用该实例）。
/// @param fnStarter 起链回调（对齐 JS executor：拿到 resolve / reject 句柄）。
/// @param eKind 本条（由外部 settle 的层）的类别（**必填**：读可并发 / 写独占 / 直投不过门）。
/// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
/// @return 指向本 promise 的句柄（pending；由 fnStarter 触发 settle）。
template <typename TContext>
CPromise<TContext> CAsyncExecutor::NewPromise(const std::shared_ptr<TContext>& spContext,
    const typename CPromise<TContext>::ChainStarter& fnStarter, TaskKind eKind, const CSourceLoc& loc /* = CSourceLoc() */)
{
    return CPromise<TContext>::NewFromHandle(Handle(), spContext, fnStarter, loc, eKind);
}

}  // namespace async
}  // namespace common
