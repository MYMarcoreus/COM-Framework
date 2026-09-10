// ====================================================================
// 基准用共享上下文与层函数（异步链特化版：层间只传成败，数据走上下文）
//
// 链 / 协程用例共用，保证各实现对同一份工作量做对比。
// ====================================================================
#ifndef COM_BENCHMARK_CASES_CHAINCONTEXT_H
#define COM_BENCHMARK_CASES_CHAINCONTEXT_H

#include <memory>

#include "Async/AsyncChain.h"
#include "Async/StepResult.h"

namespace bench {

/// @brief 基准用共享上下文（一次流程的数据载体）。
struct CChainContext
{
    long long nValue;  ///< 逐层累加值（防优化：结果被读回校验）。
    int nSteps;        ///< 已执行层数。

    CChainContext() : nValue(0), nSteps(0) {}
};

/// 层：值 +1（固定签名：上一层结果 + 共享上下文 → 本层结果）。
inline common::async::CStepResult StepInc(common::async::CStepResult upStep,
                                          const std::shared_ptr<CChainContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    spCtx->nValue += 1;
    ++spCtx->nSteps;
    return common::async::CStepResult::Ok();
}

/// 层：业务失败（用于失败即停链）。
inline common::async::CStepResult StepFail(common::async::CStepResult upStep,
                                           const std::shared_ptr<CChainContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    ++spCtx->nSteps;
    return common::async::CStepResult::Failed(common::async::kStepBusinessBase + 1);
}

}  // namespace bench

#endif  // COM_BENCHMARK_CASES_CHAINCONTEXT_H
