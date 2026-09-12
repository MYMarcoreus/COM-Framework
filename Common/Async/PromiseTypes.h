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
using SettledHandler = std::function<void(CPromiseResult result)>;

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

/// @brief 处理器（then / catch / finally 的回调，固定签名）。
///
/// 参数固定、返回固定，与上下文的具体类型解耦：
///  - upResult：上一层的结果（起链时恒为已兑现），据此判断上一层是兑现还是被拒绝；
///  - spContext：整条 promise 链共享的数据载体（恒非空）；
///  - 返回：本层结果。then / catch 以返回值决定后续走向；
///    finally 忽略返回值（原样透传上一层结果）。
template <typename TContext>
using ThenHandler = std::function<CPromiseResult(CPromiseResult upResult, const std::shared_ptr<TContext>& spContext)>;

}  // namespace detail
}  // namespace async
}  // namespace common
