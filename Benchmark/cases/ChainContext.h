// ====================================================================
// 基准用共享上下文与层函数（异步链特化版：层间只传成败，数据走上下文）
//
// 链 / 协程用例共用，保证各实现对同一份工作量做对比。
// ====================================================================
#ifndef COM_BENCHMARK_CASES_CHAINCONTEXT_H
#define COM_BENCHMARK_CASES_CHAINCONTEXT_H

#include <memory>

#include "Async/Promise.h"
#include "Async/PromiseResult.h"

namespace bench {

/// @brief 基准用业务失败文案（拒绝统一用标准异常表达）。
static const char* const kStepFailText = "基准：业务失败（用于失败即停链）";

/// @brief 基准用共享上下文（一次流程的数据载体）。
struct CChainContext
{
    long long nValue;  ///< 逐层累加值（防优化：结果被读回校验）。
    int nSteps;        ///< 已执行层数。

    CChainContext() : nValue(0), nSteps(0)
    {}
};

/// 层（then）：值 +1（固定签名：共享上下文 → 本层结果）。
inline common::async::CPromiseResult StepInc(const std::shared_ptr<CChainContext>& spCtx)
{
    spCtx->nValue += 1;
    ++spCtx->nSteps;
    return common::async::CPromiseResult::Resolve();
}

/// 层（then）：业务失败（用于失败即停链）。
inline common::async::CPromiseResult StepFail(const std::shared_ptr<CChainContext>& spCtx)
{
    ++spCtx->nSteps;
    return common::async::CPromiseResult::Reject(std::runtime_error(kStepFailText));
}

}  // namespace bench

#endif  // COM_BENCHMARK_CASES_CHAINCONTEXT_H
