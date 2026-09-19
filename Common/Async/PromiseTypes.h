#pragma once

#include <functional>
#include <memory>

#include "Async/PromiseResult.h"

// ====================================================================
// 异步 promise 公共类型
//
// 本框架只有三种对外类型（命名对齐 JS 的 Promise / async-await）：
//   CAsyncExecutor          —— 调度：线程池 + 投递（Start/Post/Stop）
//   CPromise<TContext>      —— 编排：then / catch / finally + 共享上下文
//   CCoroutine<TContext>    —— 顺序化：用顺序代码 await 多条 promise
//
// 层与层之间只传「兑现 / 拒绝」（CPromiseResult），数据一律走共享上下文。
// ====================================================================

namespace common {
namespace async {

/// @brief settled 通知：promise / 协程跑完（兑现或被拒绝）时触发一次。
///
/// 只携带最终结果；需要数据时通过 promise / 协程的 GetContext() 取共享上下文。
/// **通知不是层处理器**：它只看结果、不改结果（返回 void），名字里因此不叫 Handler。
using SettledNotice = std::function<void(CPromiseResult result)>;

namespace detail {

/// @brief 处理器模式（对应 JS 的 then / catch / finally）。
///
/// 定义放在这里（而不是 Promise.h）的原因：它是「层语义」的公共词汇 ——
/// `Async/Trace.h` 要把当前层的模式报给业务，两边都要用；放叶子头文件里可避免循环依赖。
enum HandlerMode
{
    kModeThen = 0,    ///< then(onFulfilled)：上一层兑现时执行；被拒绝则直接透传（失败即停）。
    kModeCatch = 1,   ///< catch(onRejected)：上一层被拒绝时执行；已兑现则直接透传。
    kModeFinally = 2  ///< finally(onFinally)：无论兑现或拒绝都执行；忽略返回值，透传上层结果。
};

/// @brief then 层处理器（**看不到上游结果**）。
///
/// 上一层的拒绝永远进不了 then 层 —— 框架直接跳过本层、把拒绝交给下一层（失败即停），
/// 所以形参里没有结果：既省一次多余的传参，也从签名上堵掉「在 then 里 `return upResult`」
/// 这条没有意义的路径（那本来就只可能是「已兑现」）。要处理拒绝请用 `Catch`。
///
/// @param spContext 整条 promise 链共享的数据载体（恒非空）；
/// @param 返回 本层结果（`Resolve()` = 兑现 / `Reject(异常)` = 拒绝）。
template <typename TContext>
using ThenHandler = std::function<CPromiseResult(const std::shared_ptr<TContext>& spContext)>;

/// @brief 拿到上游结果的层处理器（**catch / finally 专用**）。
///
/// 这两个模式本来就要看结果：
///  - catch（`kModeCatch`）：只在上一层**被拒绝**时执行 —— 返回 `Resolve()` 即吞掉拒绝、
///    返回 `upResult`（或任意 Reject）则继续以拒绝往下透传；
///  - finally（`kModeFinally`）：成败都执行，**忽略返回值、原样透传上游结果**。
///
/// @param upResult 上一层的结果（catch 恒为拒绝；finally 为兑现或拒绝）；
/// @param spContext 整条 promise 链共享的数据载体（恒非空）；
/// @param 返回 本层结果（finally 忽略它）。
template <typename TContext>
using ResultHandler = std::function<CPromiseResult(CPromiseResult upResult, const std::shared_ptr<TContext>& spContext)>;

}  // namespace detail
}  // namespace async
}  // namespace common
