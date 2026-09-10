#pragma once

#include <functional>
#include <memory>

#include "Async/StepResult.h"

// ====================================================================
// 异步链公共类型
//
// 本框架（异步链特化版）只有三种对外类型：
//   CAsyncExecutor         —— 调度：线程池 + 投递（Start/Post/Stop）
//   CAsyncChain<TContext>  —— 编排：固定签名层 + 共享上下文，失败即停
//   CCoroutine<TContext>   —— 顺序化：用顺序代码 await 多条链
//
// 层与层之间只传「成功 / 失败」（CStepResult），数据一律走共享上下文。
// ====================================================================

namespace common {
namespace async {

/// @brief 完成回调：链 / 协程跑完（成功或失败）时触发一次。
///
/// 只携带最终层结果；需要数据时通过链 / 协程的 GetContext() 取共享上下文。
using CCompletedFn = std::function<void(CStepResult finalStep)>;

namespace detail {

/// @brief 层函数（固定签名）：上一层结果 + 共享上下文 → 本层结果。
///
/// 参数固定、返回固定，与上下文的具体类型解耦：
///  - upStep：上一层的结果（未开始的第一层恒为成功），据此判断上一层回调；
///  - spContext：整条链共享的数据载体（恒非空）；
///  - 返回：本层结果，成功继续下一层，失败终止链。
template <typename TContext>
using StepFn = std::function<CStepResult(CStepResult upStep, const std::shared_ptr<TContext>& spContext)>;

}  // namespace detail
}  // namespace async
}  // namespace common
