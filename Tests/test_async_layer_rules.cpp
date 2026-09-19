/// @file test_async_layer_rules.cpp
/// then / catch / finally 的**三态语义**单测（黑盒：只看公开行为 —— 处理器有没有被调用、
/// 最终结果是什么）。
///
/// 判据（对齐 JS 的 Promise）：
///  - then：上游**兑现**才执行；上游被拒绝 → 本层跳过，拒绝原样交给下一层（失败即停）；
///  - catch：上游**被拒绝**才执行；已兑现 → 跳过；返回 `Resolve()` 即**恢复**（吞掉拒绝）；
///  - finally：无论兑现或拒绝**都执行**；**忽略处理器返回值**，原样透传上一层结果。
///
/// 为什么不用白盒：这三条规则直接写在各自的调用点（`AppendThenLayer` 判 `IsRejected()`、
/// `AppendResultLayer` 判 catch + `IsFulfilled()`、`MakeResultRunner` 决定 finally 的透传），
/// 模式在调用点就是常量 —— 没有「按模式分派」的公共函数可直测，所以从行为上盯住它。

#include <memory>
#include <stdexcept>
#include <string>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "TestFramework.h"

namespace {

/// @brief 三态语义用例的共享上下文（记录各层是否真的跑过、拿到的是什么结果）。
struct CLayerRuleCtx
{
    int nThenRuns;             ///< then 层执行次数。
    int nCatchRuns;            ///< catch 层执行次数。
    int nFinallyRuns;          ///< finally 层执行次数。
    bool bCatchSawRejected;    ///< catch 层拿到的是不是拒绝（catch 只可能拿到拒绝）。
    bool bFinallySawRejected;  ///< finally 层拿到的是不是拒绝（成败两种情况都要盯）。
    std::string strCatchWhat;  ///< catch 层拿到的异常描述。

    CLayerRuleCtx() : nThenRuns(0), nCatchRuns(0), nFinallyRuns(0), bCatchSawRejected(false), bFinallySawRejected(false)
    {}
};

/// @brief 首层（then）：直接兑现，用来造「上游已兑现」这条路径。
common::async::CPromiseResult StepFirst(const std::shared_ptr<CLayerRuleCtx>& spCtx)
{
    (void)spCtx;
    return common::async::CPromiseResult::Resolve();
}

/// @brief 首层（then）：直接拒绝，用来造「上游被拒绝」这条路径。
common::async::CPromiseResult StepFirstReject(const std::shared_ptr<CLayerRuleCtx>& spCtx)
{
    (void)spCtx;
    return common::async::CPromiseResult::Reject(std::runtime_error("上游拒绝（用例）"));
}

/// @brief then 层：被调用即计数（上游被拒绝时不该被调用）。
common::async::CPromiseResult StepThen(const std::shared_ptr<CLayerRuleCtx>& spCtx)
{
    ++spCtx->nThenRuns;
    return common::async::CPromiseResult::Resolve();
}

/// @brief catch 层：记下「拿到的是拒绝」并**恢复**（返回 Resolve() 吞掉拒绝）。
common::async::CPromiseResult StepCatchRecover(
    common::async::CPromiseResult upResult, const std::shared_ptr<CLayerRuleCtx>& spCtx)
{
    ++spCtx->nCatchRuns;
    spCtx->bCatchSawRejected = upResult.IsRejected();
    spCtx->strCatchWhat = upResult.Message();
    return common::async::CPromiseResult::Resolve();  // 恢复：链从本层之后继续。
}

/// @brief catch 层：**不恢复**（返回 upResult，拒绝继续往下透传）。
common::async::CPromiseResult StepCatchPassThrough(
    common::async::CPromiseResult upResult, const std::shared_ptr<CLayerRuleCtx>& spCtx)
{
    ++spCtx->nCatchRuns;
    spCtx->bCatchSawRejected = upResult.IsRejected();
    return upResult;
}

/// @brief finally 层：成败都执行，并**故意返回与上一层相反的结果**（验证返回值被忽略）。
common::async::CPromiseResult StepFinallyFlip(common::async::CPromiseResult upResult, const std::shared_ptr<CLayerRuleCtx>& spCtx)
{
    ++spCtx->nFinallyRuns;
    spCtx->bFinallySawRejected = upResult.IsRejected();
    if (upResult.IsRejected())
    {
        return common::async::CPromiseResult::Resolve();  // 想「恢复」也不生效：finally 不改结果。
    }
    return common::async::CPromiseResult::Reject(std::runtime_error("finally 里造的拒绝（应被忽略）"));
}

/// @brief 把上下文包成 promise 上下文（链的处理器只接 spCtx，上下文本身按 shared_ptr 共享）。
std::shared_ptr<CLayerRuleCtx> MakeCtx()
{
    return std::make_shared<CLayerRuleCtx>();
}

}  // namespace

/// @brief 兑现路径：then 执行、catch **跳过**、finally 执行；finally 想「造拒绝」也不生效。
TEST(LayerRules_FulfilledPath)
{
    common::async::CAsyncExecutor exec("layer-rules", 1);
    ASSERT_TRUE(exec.Start());


    // ① 串链：把要验证的三态路径串起来（上游由首层层函数决定）。
    const std::shared_ptr<CLayerRuleCtx> spCtx = MakeCtx();
    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepFirst, ASYNC_LOC)
                                                .Then(&StepThen, ASYNC_LOC)            // 上游兑现 → 执行
                                                .Catch(&StepCatchRecover, ASYNC_LOC)   // 上游兑现 → 跳过
                                                .Finally(&StepFinallyFlip, ASYNC_LOC)  // 成败都执行
                                                .Await();


    // ② 断言：谁跑了 / 谁被跳过，以及最终结果是什么。
    ASSERT_TRUE(r.IsFulfilled());       // finally 里造的拒绝没有改变结果
    ASSERT_EQ(spCtx->nThenRuns, 1);     // then 执行了
    ASSERT_EQ(spCtx->nCatchRuns, 0);    // catch 被跳过
    ASSERT_EQ(spCtx->nFinallyRuns, 1);  // finally 执行了
    ASSERT_TRUE(!spCtx->bFinallySawRejected);
    exec.Stop();
}

/// @brief 拒绝路径 ①：then **跳过**、catch 执行并**恢复** → 链从 catch 之后继续。
TEST(LayerRules_RejectedPathCatchRecovers)
{
    common::async::CAsyncExecutor exec("layer-rules", 1);
    ASSERT_TRUE(exec.Start());


    // ① 串链：把要验证的三态路径串起来（上游由首层层函数决定）。
    const std::shared_ptr<CLayerRuleCtx> spCtx = MakeCtx();
    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepFirstReject, ASYNC_LOC)
                                                .Then(&StepThen, ASYNC_LOC)           // 上游被拒绝 → 跳过
                                                .Then(&StepThen, ASYNC_LOC)           // 同上（失败即停）
                                                .Catch(&StepCatchRecover, ASYNC_LOC)  // 只在被拒绝时执行并恢复
                                                .Then(&StepThen, ASYNC_LOC)           // 已恢复 → 执行
                                                .Finally(&StepFinallyFlip, ASYNC_LOC)
                                                .Await();


    // ② 断言：谁跑了 / 谁被跳过，以及最终结果是什么。
    ASSERT_TRUE(r.IsFulfilled());           // catch 恢复 → 最终兑现
    ASSERT_EQ(spCtx->nThenRuns, 1);         // 只有「恢复之后」那一层跑过
    ASSERT_EQ(spCtx->nCatchRuns, 1);        // catch 执行了一次
    ASSERT_TRUE(spCtx->bCatchSawRejected);  // catch 一定拿到拒绝
    ASSERT_EQ(spCtx->strCatchWhat, std::string("上游拒绝（用例）"));
    ASSERT_EQ(spCtx->nFinallyRuns, 1);         // finally 照跑
    ASSERT_TRUE(!spCtx->bFinallySawRejected);  // catch 已恢复 → finally 拿到的是兑现
    exec.Stop();
}

/// @brief 拒绝路径 ②：catch **不恢复** → 拒绝原样透传；finally 照跑也改不了结果。
TEST(LayerRules_RejectedPathPassThrough)
{
    common::async::CAsyncExecutor exec("layer-rules", 1);
    ASSERT_TRUE(exec.Start());


    // ① 串链：把要验证的三态路径串起来（上游由首层层函数决定）。
    const std::shared_ptr<CLayerRuleCtx> spCtx = MakeCtx();
    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepFirstReject, ASYNC_LOC)
                                                .Then(&StepThen, ASYNC_LOC)  // 跳过
                                                .Catch(&StepCatchPassThrough, ASYNC_LOC)
                                                .Then(&StepThen, ASYNC_LOC)  // 仍是拒绝状态 → 跳过
                                                .Finally(&StepFinallyFlip, ASYNC_LOC)
                                                .Await();


    // ② 断言：谁跑了 / 谁被跳过，以及最终结果是什么。
    ASSERT_TRUE(r.IsRejected());                              // catch 没恢复 → 仍是拒绝
    ASSERT_EQ(r.Message(), std::string("上游拒绝（用例）"));  // 且是**最原始**的拒绝（透传）
    ASSERT_EQ(spCtx->nThenRuns, 0);                           // 两个 then 都没跑
    ASSERT_EQ(spCtx->nCatchRuns, 1);
    ASSERT_EQ(spCtx->nFinallyRuns, 1);
    ASSERT_TRUE(spCtx->bFinallySawRejected);  // finally 拿得到拒绝
    exec.Stop();
}
