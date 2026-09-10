/// @file test_async_chain.cpp
/// 异步链（common::async 特化版）单元测试：链 + 协程。
///
/// 被测契约：
///  - 层与层之间只传「本层成功 / 失败」（CStepResult），不传任意值；
///  - 数据一律走共享上下文 std::shared_ptr<TContext>（整条链共用同一实例）；
///  - 层函数签名固定：CStepResult(CStepResult, const std::shared_ptr<TContext>&)；
///  - 失败即停（Then）；需要「失败也执行」用 ThenAlways（回滚 / 补偿 / 清理）；
///  - 失败码透传到 Get() 与 OnCompleted，层内异常转 kStepException。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Async/AsyncChain.h"
#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"
#include "TestFramework.h"

namespace no = common::async;

// ==================== 测试用共享上下文与层函数 ====================

/// @brief 测试流程的共享上下文（链内所有层共用同一实例）。
struct CTestContext
{
    int nValue;                ///< 逐层累加的值。
    int nSteps;                ///< 已执行的层数（不含被跳过 / 未执行的层）。
    int nAlwaysRuns;           ///< ThenAlways 层执行次数。
    int nSeenOk;               ///< 观察到「上一层成功」的次数。
    int nSeenFailed;           ///< 观察到「上一层失败」的次数。
    int nFailCode;             ///< 制造成败用的错误码（0 表示成功）。
    std::thread::id workerId;  ///< 最后一个执行层的工作线程 id。
    std::string strTrace;      ///< 层执行轨迹（每层一个字符）。

    CTestContext() : nValue(0), nSteps(0), nAlwaysRuns(0), nSeenOk(0), nSeenFailed(0), nFailCode(0) {}
};

/// 层：值 +1（上一层失败则透传，属防御性写法）。
static no::CStepResult StepAdd1(no::CStepResult upStep, const std::shared_ptr<CTestContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    ++spCtx->nValue;
    ++spCtx->nSteps;
    spCtx->workerId = std::this_thread::get_id();
    spCtx->strTrace += "1";
    return no::CStepResult::Ok();
}

/// 层：值 +10。
static no::CStepResult StepAdd10(no::CStepResult upStep, const std::shared_ptr<CTestContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    spCtx->nValue += 10;
    ++spCtx->nSteps;
    spCtx->workerId = std::this_thread::get_id();
    spCtx->strTrace += "2";
    return no::CStepResult::Ok();
}

/// 层：按上下文里的错误码制造失败。
static no::CStepResult StepFail(no::CStepResult upStep, const std::shared_ptr<CTestContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    ++spCtx->nSteps;
    spCtx->strTrace += "F";
    return no::CStepResult::Failed(spCtx->nFailCode);
}

/// 层：被调用即留下痕迹（用于验证失败后不再执行）。
static no::CStepResult StepShouldNotRun(no::CStepResult upStep, const std::shared_ptr<CTestContext>& spCtx)
{
    (void)upStep;
    ++spCtx->nSteps;
    spCtx->strTrace += "X";
    return no::CStepResult::Ok();
}

/// 层：抛异常（验证框架捕获 → kStepException，不向调用方抛出）。
static no::CStepResult StepThrow(no::CStepResult /*upStep*/, const std::shared_ptr<CTestContext>& /*spCtx*/)
{
    throw std::runtime_error("chain step boom");
}

/// 层（ThenAlways）：观察上一层成败并透传（回滚 / 清理的典型写法）。
static no::CStepResult StepAlwaysObserve(no::CStepResult upStep, const std::shared_ptr<CTestContext>& spCtx)
{
    ++spCtx->nAlwaysRuns;
    if (upStep.IsOk())
    {
        ++spCtx->nSeenOk;
    }
    else
    {
        ++spCtx->nSeenFailed;
    }
    spCtx->strTrace += "A";
    return upStep;  // 透传：失败继续失败，成功继续成功。
}

/// 层（ThenAlways）：吞掉失败并恢复链（后续 Then 层会重新执行）。
static no::CStepResult StepAlwaysRecover(no::CStepResult /*upStep*/, const std::shared_ptr<CTestContext>& spCtx)
{
    ++spCtx->nAlwaysRuns;
    spCtx->strTrace += "R";
    return no::CStepResult::Ok();
}

// ==================== 契约与基本链路 ====================

/// @brief 固定签名契约：层函数 / 完成回调必须能赋给框架声明的函数类型。
TEST(AsyncChain_StepFnContract)
{
    // 层函数：CStepResult(CStepResult, const std::shared_ptr<TContext>&)。
    no::CAsyncChain<CTestContext>::StepFn fnStep = &StepAdd1;
    ASSERT_TRUE(static_cast<bool>(fnStep));

    // 完成回调：void(CStepResult)。
    no::CAsyncChain<CTestContext>::CompletedFn fnCompleted = [](no::CStepResult) {};
    ASSERT_TRUE(static_cast<bool>(fnCompleted));

    // 层结果码：框架保留区间 + 业务码起始值。
    ASSERT_EQ(static_cast<int>(no::kStepOk), 0);
    ASSERT_TRUE(no::kStepBusinessBase >= 100);
    ASSERT_TRUE(no::CStepResult::Ok().IsOk());
    ASSERT_TRUE(no::CStepResult::Failed(no::kStepBusinessBase).IsFailed());
}

/// @brief 单层链：Submit → Get（成功）。
TEST(AsyncChain_SubmitAndGet)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    no::CAsyncChain<CTestContext> chain = exec.Submit(spCtx, &StepAdd1, ASYNC_LOC);
    const no::CStepResult r = chain.Get();

    ASSERT_TRUE(r.IsOk());
    ASSERT_EQ(r.Code(), static_cast<int>(no::kStepOk));
    ASSERT_EQ(spCtx->nValue, 1);
    ASSERT_EQ(spCtx->nSteps, 1);
    exec.Stop();
}

/// @brief 多层链顺序执行：数据经共享上下文传递，层间只传成败。
TEST(AsyncChain_ThenSequence)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    no::CAsyncChain<CTestContext> tail =
        exec.Submit(spCtx, &StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC).Then(&StepAdd1, ASYNC_LOC);
    const no::CStepResult r = tail.Get();

    ASSERT_TRUE(r.IsOk());
    ASSERT_EQ(spCtx->nValue, 12);  // 1 + 10 + 1
    ASSERT_EQ(spCtx->nSteps, 3);
    ASSERT_EQ(spCtx->strTrace, std::string("121"));  // 顺序确定：1 → 2 → 1
    exec.Stop();
}

/// @brief 懒创建上下文：链自己创建，外部取用后填初始数据。
TEST(AsyncChain_LazyContext)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    no::CAsyncChain<CTestContext> chain(exec);
    ASSERT_TRUE(chain.GetContext() != nullptr);  // 懒创建，恒非空
    chain.GetContext()->nValue = 100;

    no::CAsyncChain<CTestContext> tail = chain.Submit(&StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC);
    ASSERT_TRUE(tail.Get().IsOk());
    ASSERT_EQ(chain.GetContext()->nValue, 111);
    exec.Stop();
}

/// @brief 外部注入上下文：链内所有层共用外部实例。
TEST(AsyncChain_ExternalContext)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    no::CAsyncChain<CTestContext> chain(exec, spCtx);
    ASSERT_TRUE(chain.GetContext() == spCtx);  // 同一实例，不做拷贝

    ASSERT_TRUE(chain.Submit(&StepAdd10, ASYNC_LOC).Get().IsOk());
    ASSERT_EQ(spCtx->nValue, 10);
    exec.Stop();
}

// ==================== 失败语义 ====================

/// @brief 失败即停：失败层之后的 Then 层不执行，失败码透传到 Get()。
TEST(AsyncChain_FailFast)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->nFailCode = no::kStepBusinessBase + 7;

    no::CAsyncChain<CTestContext> tail = exec.Submit(spCtx, &StepAdd1, ASYNC_LOC)
                                             .Then(&StepFail, ASYNC_LOC)
                                             .Then(&StepShouldNotRun, ASYNC_LOC);  // 不执行
    const no::CStepResult r = tail.Get();

    ASSERT_TRUE(r.IsFailed());
    ASSERT_EQ(r.Code(), no::kStepBusinessBase + 7);  // 业务码原样透传
    ASSERT_EQ(spCtx->nValue, 1);                     // 只跑了第一层
    ASSERT_EQ(spCtx->strTrace, std::string("1F"));   // 没有 "X"
    exec.Stop();
}

/// @brief 层内异常 → 本层失败（kStepException），框架不向调用方抛出。
TEST(AsyncChain_ExceptionToFailed)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    no::CAsyncChain<CTestContext> tail = exec.Submit(spCtx, &StepThrow, ASYNC_LOC).Then(&StepShouldNotRun, ASYNC_LOC);
    const no::CStepResult r = tail.Get();

    ASSERT_TRUE(r.IsFailed());
    ASSERT_EQ(r.Code(), static_cast<int>(no::kStepException));
    ASSERT_EQ(spCtx->nSteps, 0);  // 异常层与后续层都没留下业务痕迹
    exec.Stop();
}

/// @brief ThenAlways：失败时仍执行，upStep 携带失败状态（可回滚），返回 upStep 继续透传。
TEST(AsyncChain_ThenAlwaysSeesFailure)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->nFailCode = no::kStepBusinessBase + 3;

    no::CAsyncChain<CTestContext> tail = exec.Submit(spCtx, &StepFail, ASYNC_LOC)
                                             .ThenAlways(&StepAlwaysObserve, ASYNC_LOC)
                                             .Then(&StepShouldNotRun, ASYNC_LOC);  // 失败仍在，不执行
    const no::CStepResult r = tail.Get();

    ASSERT_TRUE(r.IsFailed());
    ASSERT_EQ(r.Code(), no::kStepBusinessBase + 3);
    ASSERT_EQ(spCtx->nAlwaysRuns, 1);
    ASSERT_EQ(spCtx->nSeenFailed, 1);  // 真的看到了上一层的失败
    ASSERT_EQ(spCtx->nSeenOk, 0);
    ASSERT_EQ(spCtx->strTrace, std::string("FA"));  // 失败层 + ThenAlways 层
    exec.Stop();
}

/// @brief ThenAlways 返回成功 → 吞掉失败，链从本层之后继续执行。
TEST(AsyncChain_ThenAlwaysRecover)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->nFailCode = no::kStepBusinessBase + 5;

    no::CAsyncChain<CTestContext> tail = exec.Submit(spCtx, &StepFail, ASYNC_LOC)
                                             .ThenAlways(&StepAlwaysRecover, ASYNC_LOC)
                                             .Then(&StepAdd1, ASYNC_LOC);  // 失败已被吞掉 → 执行
    const no::CStepResult r = tail.Get();

    ASSERT_TRUE(r.IsOk());  // 链恢复
    ASSERT_EQ(spCtx->nAlwaysRuns, 1);
    ASSERT_EQ(spCtx->nValue, 1);
    ASSERT_EQ(spCtx->strTrace, std::string("FR1"));  // 失败 → 恢复 → 继续
    exec.Stop();
}

/// @brief OnCompleted：成功与失败都触发一次（携带最终结果）。
TEST(AsyncChain_CompletedCallback)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    // 成功链
    std::shared_ptr<CTestContext> spOk = std::make_shared<CTestContext>();
    std::atomic<int> nOk(0);
    std::atomic<bool> bOkDone(false);
    no::CAsyncChain<CTestContext> chainOk = exec.Submit(spOk, &StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC);
    ASSERT_TRUE(chainOk.OnCompleted([&nOk, &bOkDone](no::CStepResult r)
    {
        nOk.store(r.Code() + 1);  // 成功：0 + 1
        bOkDone.store(true);
    }));
    chainOk.Get();
    while (!bOkDone.load())
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(nOk.load(), 1);  // Code()==kStepOk

    // 失败链
    std::shared_ptr<CTestContext> spFail = std::make_shared<CTestContext>();
    spFail->nFailCode = no::kStepBusinessBase + 9;
    std::atomic<int> nCode(-1);
    std::atomic<bool> bFailDone(false);
    no::CAsyncChain<CTestContext> chainFail = exec.Submit(spFail, &StepFail, ASYNC_LOC);
    ASSERT_TRUE(chainFail.OnCompleted([&nCode, &bFailDone](no::CStepResult r)
    {
        nCode.store(r.Code());
        bFailDone.store(true);
    }));
    chainFail.Get();
    while (!bFailDone.load())
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(nCode.load(), no::kStepBusinessBase + 9);
    exec.Stop();
}

// ==================== 注册时机与分叉 ====================

/// @brief 分叉：同一层注册两个 Then，各自独立延续。
TEST(AsyncChain_Fork)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    no::CAsyncChain<CTestContext> head = exec.Submit(spCtx, &StepAdd1, ASYNC_LOC);

    std::atomic<int> nDone(0);
    no::CAsyncChain<CTestContext> branchA = head.Then(&StepAdd1, ASYNC_LOC);
    no::CAsyncChain<CTestContext> branchB = head.Then(&StepAdd10, ASYNC_LOC);
    branchA.OnCompleted([&nDone](no::CStepResult) { nDone.fetch_add(1); });
    branchB.OnCompleted([&nDone](no::CStepResult) { nDone.fetch_add(1); });

    ASSERT_TRUE(branchA.Get().IsOk());
    ASSERT_TRUE(branchB.Get().IsOk());
    while (nDone.load() < 2)
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(spCtx->nValue, 12);  // 1（首层）+ 1 + 10
    exec.Stop();
}

/// @brief 链完成后再追加层：投递到执行器异步执行（不阻塞调用方）。
TEST(AsyncChain_ThenAfterCompleted)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    no::CAsyncChain<CTestContext> chain = exec.Submit(spCtx, &StepAdd1, ASYNC_LOC);
    ASSERT_TRUE(chain.Get().IsOk());  // 首层已完成

    std::atomic<bool> bDone(false);
    no::CAsyncChain<CTestContext> tail = chain.Then(&StepAdd10, ASYNC_LOC);
    tail.OnCompleted([&bDone](no::CStepResult) { bDone.store(true); });

    ASSERT_TRUE(tail.Get().IsOk());
    while (!bDone.load())
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(spCtx->nValue, 11);
    exec.Stop();
}

// ==================== 执行器生命周期与线程模型 ====================

/// @brief 未启动执行器起链 → 立即失败（kStepStopped），不阻塞。
TEST(AsyncChain_NotStarted)
{
    no::CAsyncExecutor exec(2);
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();

    no::CAsyncChain<CTestContext> tail = exec.Submit(spCtx, &StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC);
    const no::CStepResult r = tail.Get();

    ASSERT_TRUE(r.IsFailed());
    ASSERT_EQ(r.Code(), static_cast<int>(no::kStepStopped));
    ASSERT_EQ(spCtx->nSteps, 0);
}

/// @brief Stop 之后起链 → 失败（kStepStopped）。
TEST(AsyncChain_SubmitAfterStop)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());
    exec.Stop();

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    const no::CStepResult r = exec.Submit(spCtx, &StepAdd1, ASYNC_LOC).Get();
    ASSERT_TRUE(r.IsFailed());
    ASSERT_EQ(r.Code(), static_cast<int>(no::kStepStopped));
}

/// @brief Stop 之后重新 Start：可继续起链。
TEST(AsyncChain_RestartExecutor)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx1 = std::make_shared<CTestContext>();
    ASSERT_TRUE(exec.Submit(spCtx1, &StepAdd1, ASYNC_LOC).Get().IsOk());
    exec.Stop();

    ASSERT_TRUE(exec.Start());  // 重新启动（重建句柄与线程池）
    std::shared_ptr<CTestContext> spCtx2 = std::make_shared<CTestContext>();
    ASSERT_TRUE(exec.Submit(spCtx2, &StepAdd1, ASYNC_LOC).Get().IsOk());
    ASSERT_EQ(spCtx2->nValue, 1);
    exec.Stop();
}

/// @brief 层在工作线程上执行（不在起链线程）。
TEST(AsyncChain_WorkerThread)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    const std::thread::id mainId = std::this_thread::get_id();
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    ASSERT_TRUE(exec.Submit(spCtx, &StepAdd1, ASYNC_LOC).Get().IsOk());
    ASSERT_TRUE(spCtx->workerId != mainId);
    exec.Stop();
}

/// @brief 执行器析构后，已起的链仍安全完成（句柄保活线程池）。
TEST(AsyncChain_LifetimeAfterDestroy)
{
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    no::CAsyncChain<CTestContext> tail;
    {
        no::CAsyncExecutor exec(2);
        ASSERT_TRUE(exec.Start());
        tail = exec.Submit(spCtx, &StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC);
        exec.Stop();  // 等待已投递任务完成
    }  // 执行器析构

    const no::CStepResult r = tail.Get();
    ASSERT_TRUE(r.IsOk());
    ASSERT_EQ(spCtx->nValue, 11);
}

/// @brief 多线程等待同一链（并发 Get）。
TEST(AsyncChain_ConcurrentGet)
{
    no::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    no::CAsyncChain<CTestContext> tail = exec.Submit(spCtx, &StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC);

    std::atomic<int> nOk(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
    {
        threads.push_back(std::thread([&tail, &nOk]()
        {
            if (tail.Get().IsOk())
            {
                nOk.fetch_add(1);
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    ASSERT_EQ(nOk.load(), 8);
    ASSERT_EQ(spCtx->nSteps, 2);  // 层不会被重复执行
    exec.Stop();
}

/// @brief 多条链并行执行（不被串行化）。
TEST(AsyncChain_ParallelChains)
{
    no::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nActive(0);
    std::atomic<int> nPeak(0);
    const int kChains = 8;

    std::vector<no::CAsyncChain<CTestContext> > chains;
    for (int i = 0; i < kChains; ++i)
    {
        std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
        no::CAsyncChain<CTestContext> chain = exec.Submit(
            spCtx, [&nActive, &nPeak](no::CStepResult /*upStep*/, const std::shared_ptr<CTestContext>& /*spCtx*/)
        {
            const int nNow = nActive.fetch_add(1) + 1;
            int nCur = nPeak.load();
            while (nCur < nNow && !nPeak.compare_exchange_weak(nCur, nNow))
            {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            nActive.fetch_sub(1);
            return no::CStepResult::Ok();
        }, ASYNC_LOC);
        chains.push_back(chain);
    }

    for (size_t i = 0; i < chains.size(); ++i)
    {
        ASSERT_TRUE(chains[i].Get().IsOk());
    }
    ASSERT_TRUE(nPeak.load() >= 2);  // 4 线程下多条链应并行（而非串行）
    exec.Stop();
}

/// @brief 深链：层数超过内联深度上限仍正确执行（超限改投递，防爆栈）。
TEST(AsyncChain_DeepChain)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    const int kLayers = 300;
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    no::CAsyncChain<CTestContext> tail = exec.Submit(spCtx, &StepAdd1, ASYNC_LOC);
    for (int i = 1; i < kLayers; ++i)
    {
        tail = tail.Then(&StepAdd1, ASYNC_LOC);
    }
    const no::CStepResult r = tail.Get();

    ASSERT_TRUE(r.IsOk());
    ASSERT_EQ(spCtx->nSteps, kLayers);
    ASSERT_EQ(spCtx->nValue, kLayers);
    exec.Stop();
}

/// @brief 压力：大量链 × 多层，全部完成且计数精确。
TEST(AsyncChain_Stress)
{
    no::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const int kChains = 400;
    std::atomic<long> nSteps(0);
    std::vector<no::CAsyncChain<CTestContext> > tails;
    tails.reserve(kChains);
    for (int i = 0; i < kChains; ++i)
    {
        std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
        auto step = [&nSteps](no::CStepResult upStep, const std::shared_ptr<CTestContext>& sp)
        {
            if (upStep.IsFailed())
            {
                return upStep;
            }
            ++sp->nSteps;
            nSteps.fetch_add(1);
            return no::CStepResult::Ok();
        };
        tails.push_back(exec.Submit(spCtx, step, ASYNC_LOC).Then(step, ASYNC_LOC).Then(step, ASYNC_LOC));
    }

    int nOk = 0;
    for (size_t i = 0; i < tails.size(); ++i)
    {
        if (tails[i].Get().IsOk())
        {
            ++nOk;
        }
    }
    ASSERT_EQ(nOk, kChains);
    ASSERT_EQ(nSteps.load(), static_cast<long>(kChains) * 3);
    exec.Stop();
}

/// @brief Post：未启动 / 已停止时不接受；启动后任务在工作线程执行。
TEST(AsyncChain_PostBehavior)
{
    no::CAsyncExecutor exec(2);
    std::atomic<int> nDone(0);
    ASSERT_TRUE(!exec.Post([&nDone]() { nDone.fetch_add(1); }));  // 未启动

    ASSERT_TRUE(exec.Start());
    const std::thread::id mainId = std::this_thread::get_id();
    std::thread::id workerId;
    ASSERT_TRUE(exec.Post([&nDone, &workerId, mainId]()
    {
        workerId = std::this_thread::get_id();
        (void)mainId;
        nDone.fetch_add(1);
    }));
    exec.Stop();  // 等待任务完成
    ASSERT_EQ(nDone.load(), 1);
    ASSERT_TRUE(workerId != mainId);

    ASSERT_TRUE(!exec.Post([&nDone]() { nDone.fetch_add(1); }));  // 已停止
    ASSERT_EQ(nDone.load(), 1);
}

// ==================== 协程 ====================

/// 协程：顺序 await 两条子链（协程与子链共享同一上下文）。
class CSequentialCoro : public no::CCoroutine<CTestContext>
{
   public:
    using no::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(Chain(&StepAdd1));
        CO_AWAIT(Chain(&StepAdd10));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：并行 await 两条子链。
class CParallelCoro : public no::CCoroutine<CTestContext>
{
   public:
    using no::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL(Chain(&StepAdd1), Chain(&StepAdd10));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：await 到失败 → 终止（失败码透传，后续 await 不执行）。
class CFailCoro : public no::CCoroutine<CTestContext>
{
   public:
    using no::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(Chain(&StepFail));
        CO_AWAIT(Chain(&StepShouldNotRun));  // 不执行
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：显式以失败结果结束（CO_RETURN）。
class CReturnFailCoro : public no::CCoroutine<CTestContext>
{
   public:
    using no::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(Chain(&StepAdd1));
        CO_RETURN(no::CStepResult::Failed(no::kStepBusinessBase + 11));
        CO_END();
    }
};

/// 子协程：await 一条子链。
class CChildCoro : public no::CCoroutine<CTestContext>
{
   public:
    using no::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(Chain(&StepAdd10));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 父协程：await 子协程（嵌套）。
class CParentCoro : public no::CCoroutine<CTestContext>
{
   public:
    explicit CParentCoro(const std::shared_ptr<CTestContext>& spCtx, no::CAsyncExecutor* pExec)
        : no::CCoroutine<CTestContext>(spCtx), m_pExec(pExec), m_pChild()
    {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(Chain(&StepAdd1));
        m_pChild = m_pExec->CoStart<CChildCoro>(GetContext());  // 跨 await 的变量须为成员
        CO_AWAIT(m_pChild->AsChain());
        CO_RETURN_VOID();
        CO_END();
    }

   private:
    no::CAsyncExecutor* m_pExec;
    std::shared_ptr<CChildCoro> m_pChild;
};

/// @brief 协程顺序 await：子链共用协程上下文，最终结果为成功。
TEST(AsyncChainCoro_Sequential)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CSequentialCoro> pCoro = exec.CoStart<CSequentialCoro>(spCtx);

    ASSERT_TRUE(pCoro->Get().IsOk());
    ASSERT_EQ(spCtx->nValue, 11);                   // 1 + 10
    ASSERT_EQ(spCtx->strTrace, std::string("12"));  // 顺序确定
    ASSERT_TRUE(pCoro->GetContext() == spCtx);      // 协程与子链共享同一上下文
    exec.Stop();
}

/// @brief 协程并行 await（CO_AWAIT_ALL）。
TEST(AsyncChainCoro_Parallel)
{
    no::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CParallelCoro> pCoro = exec.CoStart<CParallelCoro>(spCtx);

    ASSERT_TRUE(pCoro->Get().IsOk());
    ASSERT_EQ(spCtx->nValue, 11);  // 1 + 10（并行，顺序不定）
    exec.Stop();
}

/// @brief 协程 await 到失败 → 协程终止，失败码透传，后续 await 不执行。
TEST(AsyncChainCoro_AwaitFailed)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->nFailCode = no::kStepBusinessBase + 4;
    std::shared_ptr<CFailCoro> pCoro = exec.CoStart<CFailCoro>(spCtx);

    const no::CStepResult r = pCoro->Get();
    ASSERT_TRUE(r.IsFailed());
    ASSERT_EQ(r.Code(), no::kStepBusinessBase + 4);
    ASSERT_EQ(spCtx->strTrace, std::string("F"));  // 后续层未执行
    exec.Stop();
}

/// @brief 协程显式以失败结束（CO_RETURN(Failed(...))）。
TEST(AsyncChainCoro_ReturnFailed)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CReturnFailCoro> pCoro = exec.CoStart<CReturnFailCoro>(spCtx);

    const no::CStepResult r = pCoro->Get();
    ASSERT_TRUE(r.IsFailed());
    ASSERT_EQ(r.Code(), no::kStepBusinessBase + 11);
    ASSERT_EQ(spCtx->nValue, 1);  // await 的子链已执行
    exec.Stop();
}

/// @brief 嵌套：协程 await 子协程（AsChain），共用同一上下文。
TEST(AsyncChainCoro_Nested)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CParentCoro> pCoro = exec.CoStart<CParentCoro>(spCtx, &exec);

    ASSERT_TRUE(pCoro->Get().IsOk());
    ASSERT_EQ(spCtx->nValue, 11);                   // 父 1 + 子 10
    ASSERT_EQ(spCtx->strTrace, std::string("12"));  // 顺序确定
    exec.Stop();
}

/// @brief 未启动执行器起协程 → 立即失败（kStepStopped），Get 不阻塞。
TEST(AsyncChainCoro_NotStarted)
{
    no::CAsyncExecutor exec(2);
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CSequentialCoro> pCoro = exec.CoStart<CSequentialCoro>(spCtx);

    const no::CStepResult r = pCoro->Get();
    ASSERT_TRUE(r.IsFailed());
    ASSERT_EQ(r.Code(), static_cast<int>(no::kStepStopped));
}

/// @brief 协程可重复启动（Reset 后重新执行）。
TEST(AsyncChainCoro_Restart)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CSequentialCoro> pCoro = exec.CoStart<CSequentialCoro>(spCtx);
    ASSERT_TRUE(pCoro->Get().IsOk());
    ASSERT_EQ(spCtx->nValue, 11);

    pCoro->Start(&exec);  // 复用同一协程对象再次执行
    ASSERT_TRUE(pCoro->Get().IsOk());
    ASSERT_EQ(spCtx->nValue, 22);
    exec.Stop();
}

/// @brief 协程作为可等待对象注册完成回调（AsChain + OnCompleted）。
TEST(AsyncChainCoro_CompletedCallback)
{
    no::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CSequentialCoro> pCoro = exec.CoStart<CSequentialCoro>(spCtx);

    std::atomic<int> nCode(-1);
    ASSERT_TRUE(pCoro->AsChain().OnCompleted([&nCode](no::CStepResult r) { nCode.store(r.Code()); }));
    while (nCode.load() < 0)
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(nCode.load(), static_cast<int>(no::kStepOk));
    exec.Stop();
}
