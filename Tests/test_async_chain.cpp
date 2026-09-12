/// @file test_async_chain.cpp
/// 异步 promise（common::async 特化版）单元测试：promise + 协程。
///
/// 被测契约：
///  - 层与层之间只传「兑现 / 拒绝」（CPromiseResult），不传任意值；
///  - 数据一律走共享上下文 std::shared_ptr<TContext>（整条 promise 链共用同一实例）；
///  - 处理器签名固定：CPromiseResult(CPromiseResult, const std::shared_ptr<TContext>&)；
///  - then：失败即停；catch：仅被拒绝时执行（Resolve() 即恢复）；
///    finally：无论成败都执行且不改结果（与 JS 的 finally 一致）；
///  - 拒绝码透传到 Await() 与 OnSettled，处理器内异常转 kException。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Async/Coroutine.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "TestFramework.h"

// ==================== 测试用共享上下文与层函数 ====================

/// @brief 测试流程的共享上下文（链内所有层共用同一实例）。
struct CTestContext
{
    int nValue;                ///< 逐层累加的值。
    int nSteps;                ///< 已执行的层数（不含被跳过 / 未执行的层）。
    int nCatchRuns;            ///< catch 层执行次数。
    int nSeenOk;               ///< 观察到「上一层成功」的次数。
    int nSeenFailed;           ///< 观察到「上一层失败」的次数。
    int nFailCode;             ///< 制造成败用的错误码（0 表示成功）。
    std::thread::id workerId;  ///< 最后一个执行层的工作线程 id。
    std::string strTrace;      ///< 层执行轨迹（每层一个字符）。

    // 分叉用例专用：两条分支并发跑，按「共用上下文只写不同字段」的契约各写自己的一格。
    std::atomic<int> nForkA;  ///< 分叉分支 A 的执行次数。
    std::atomic<int> nForkB;  ///< 分叉分支 B 的执行次数。

    CTestContext() : nValue(0), nSteps(0), nCatchRuns(0), nSeenOk(0), nSeenFailed(0), nFailCode(0), nForkA(0), nForkB(0)
    {}
};

/// 层：值 +1（上一层失败则透传，属防御性写法）。
static common::async::CPromiseResult StepAdd1(
    common::async::CPromiseResult upResult, const std::shared_ptr<CTestContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    ++spCtx->nValue;
    ++spCtx->nSteps;
    spCtx->workerId = std::this_thread::get_id();
    spCtx->strTrace += "1";
    return common::async::CPromiseResult::Resolve();
}

/// 层：值 +10。
static common::async::CPromiseResult StepAdd10(
    common::async::CPromiseResult upResult, const std::shared_ptr<CTestContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    spCtx->nValue += 10;
    ++spCtx->nSteps;
    spCtx->workerId = std::this_thread::get_id();
    spCtx->strTrace += "2";
    return common::async::CPromiseResult::Resolve();
}

/// 分叉用例专用层：分支 A 只写自己的字段（不碰分支 B 也会写的字段）。
static common::async::CPromiseResult StepForkA(
    common::async::CPromiseResult upResult, const std::shared_ptr<CTestContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    spCtx->nForkA.fetch_add(1);
    return common::async::CPromiseResult::Resolve();
}

/// 分叉用例专用层：分支 B 只写自己的字段。
static common::async::CPromiseResult StepForkB(
    common::async::CPromiseResult upResult, const std::shared_ptr<CTestContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    spCtx->nForkB.fetch_add(1);
    return common::async::CPromiseResult::Resolve();
}

/// 层：按上下文里的错误码制造失败。
static common::async::CPromiseResult StepFail(
    common::async::CPromiseResult upResult, const std::shared_ptr<CTestContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    ++spCtx->nSteps;
    spCtx->strTrace += "F";
    return common::async::CPromiseResult::Reject(spCtx->nFailCode);
}

/// 层：被调用即留下痕迹（用于验证失败后不再执行）。
static common::async::CPromiseResult StepShouldNotRun(
    common::async::CPromiseResult upResult, const std::shared_ptr<CTestContext>& spCtx)
{
    (void)upResult;
    ++spCtx->nSteps;
    spCtx->strTrace += "X";
    return common::async::CPromiseResult::Resolve();
}

/// 层：抛异常（验证框架捕获 → kException，不向调用方抛出）。
static common::async::CPromiseResult StepThrow(
    common::async::CPromiseResult /*upStep*/, const std::shared_ptr<CTestContext>& /*spCtx*/)
{
    throw std::runtime_error("chain step boom");
}

/// 层（catch）：观察上一层拒绝并透传（回滚 / 清理的典型写法）。
static common::async::CPromiseResult StepCatchObserve(
    common::async::CPromiseResult upResult, const std::shared_ptr<CTestContext>& spCtx)
{
    ++spCtx->nCatchRuns;
    if (upResult.IsFulfilled())
    {
        ++spCtx->nSeenOk;
    }
    else
    {
        ++spCtx->nSeenFailed;
    }
    spCtx->strTrace += "A";
    return upResult;  // 透传：拒绝继续拒绝，兑现继续兑现。
}

/// 层（catch）：吞掉拒绝并恢复链（后续 then 层会重新执行）。
static common::async::CPromiseResult StepCatchRecover(
    common::async::CPromiseResult /*upStep*/, const std::shared_ptr<CTestContext>& spCtx)
{
    ++spCtx->nCatchRuns;
    spCtx->strTrace += "R";
    return common::async::CPromiseResult::Resolve();
}

// ==================== 契约与基本链路 ====================

/// @brief 固定签名契约：层函数 / 完成回调必须能赋给框架声明的函数类型。
TEST(Promise_ThenHandlerContract)
{
    // 处理器：CPromiseResult(CPromiseResult, const std::shared_ptr<TContext>&)。
    common::async::CPromise<CTestContext>::ThenHandler fnStep = &StepAdd1;
    ASSERT_TRUE(static_cast<bool>(fnStep));

    // settled 通知：void(CPromiseResult)。
    common::async::SettledHandler fnSettled = [](common::async::CPromiseResult)
    {
    };
    ASSERT_TRUE(static_cast<bool>(fnSettled));

    // 层结果码：框架保留区间 + 业务码起始值。
    ASSERT_EQ(static_cast<int>(common::async::kFulfilled), 0);
    ASSERT_TRUE(common::async::kBusinessBase >= 100);
    ASSERT_TRUE(common::async::CPromiseResult::Resolve().IsFulfilled());
    ASSERT_TRUE(common::async::CPromiseResult::Reject(common::async::kBusinessBase).IsRejected());
}

/// @brief 单层 promise：NewPromise → Await（兑现）。
TEST(Promise_NewPromiseAndAwait)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> chain = exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC);
    const common::async::CPromiseResult r = chain.Await();

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(r.Code(), static_cast<int>(common::async::kFulfilled));
    ASSERT_EQ(spCtx->nValue, 1);
    ASSERT_EQ(spCtx->nSteps, 1);
    exec.Stop();
}

/// @brief 多层链顺序执行：数据经共享上下文传递，层间只传成败。
TEST(Promise_ThenSequence)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> tail =
        exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC).Then(&StepAdd1, ASYNC_LOC);
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 12);  // 1 + 10 + 1
    ASSERT_EQ(spCtx->nSteps, 3);
    ASSERT_EQ(spCtx->strTrace, std::string("121"));  // 顺序确定：1 → 2 → 1
    exec.Stop();
}

/// @brief 懒创建上下文：链自己创建，外部取用后填初始数据。
TEST(Promise_LazyContext)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    common::async::CPromise<CTestContext> chain = exec.BuildPromise<CTestContext>();  // 上下文懒创建
    ASSERT_TRUE(chain.GetContext() != nullptr);                                       // 懒创建，恒非空
    chain.GetContext()->nValue = 100;

    common::async::CPromise<CTestContext> tail = chain.Then(&StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC);
    ASSERT_TRUE(tail.Await().IsFulfilled());
    ASSERT_EQ(chain.GetContext()->nValue, 111);
    exec.Stop();
}

/// @brief 外部注入上下文：链内所有层共用外部实例。
TEST(Promise_ExternalContext)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> chain = exec.BuildPromise<CTestContext>(spCtx);
    ASSERT_TRUE(chain.GetContext() == spCtx);  // 同一实例，不做拷贝

    ASSERT_TRUE(chain.Then(&StepAdd10, ASYNC_LOC).Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 10);
    exec.Stop();
}

// ==================== 失败语义 ====================

/// @brief 失败即停（then）：失败层之后的 then 层不执行，拒绝码透传到 Await()。
TEST(Promise_ThenFailFast)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->nFailCode = common::async::kBusinessBase + 7;

    common::async::CPromise<CTestContext> tail = exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC)
                                                     .Then(&StepFail, ASYNC_LOC)
                                                     .Then(&StepShouldNotRun, ASYNC_LOC);  // 不执行
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Code(), common::async::kBusinessBase + 7);  // 业务码原样透传
    ASSERT_EQ(spCtx->nValue, 1);                            // 只跑了第一层
    ASSERT_EQ(spCtx->strTrace, std::string("1F"));          // 没有 "X"
    exec.Stop();
}

/// @brief 处理器内异常 → 本层被拒绝（kException），框架不向调用方抛出。
TEST(Promise_ExceptionRejects)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> tail =
        exec.NewPromise(spCtx, &StepThrow, ASYNC_LOC).Then(&StepShouldNotRun, ASYNC_LOC);
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Code(), static_cast<int>(common::async::kException));
    ASSERT_EQ(spCtx->nSteps, 0);  // 异常层与后续层都没留下业务痕迹
    exec.Stop();
}

/// @brief catch：上一层被拒绝时执行，upResult 携带拒绝结果（可回滚），返回 upResult 继续透传。
TEST(Promise_CatchSeesRejection)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->nFailCode = common::async::kBusinessBase + 3;

    common::async::CPromise<CTestContext> tail = exec.NewPromise(spCtx, &StepFail, ASYNC_LOC)
                                                     .Catch(&StepCatchObserve, ASYNC_LOC)
                                                     .Then(&StepShouldNotRun, ASYNC_LOC);  // 失败仍在，不执行
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Code(), common::async::kBusinessBase + 3);
    ASSERT_EQ(spCtx->nCatchRuns, 1);
    ASSERT_EQ(spCtx->nSeenFailed, 1);  // 真的看到了上一层的失败
    ASSERT_EQ(spCtx->nSeenOk, 0);
    ASSERT_EQ(spCtx->strTrace, std::string("FA"));  // 被拒绝层 + catch 层
    exec.Stop();
}

/// @brief catch 返回 Resolve() → 吞掉拒绝，promise 从本层之后继续执行。
TEST(Promise_CatchRecover)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->nFailCode = common::async::kBusinessBase + 5;

    common::async::CPromise<CTestContext> tail = exec.NewPromise(spCtx, &StepFail, ASYNC_LOC)
                                                     .Catch(&StepCatchRecover, ASYNC_LOC)
                                                     .Then(&StepAdd1, ASYNC_LOC);  // 失败已被吞掉 → 执行
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsFulfilled());  // 链恢复
    ASSERT_EQ(spCtx->nCatchRuns, 1);
    ASSERT_EQ(spCtx->nValue, 1);
    ASSERT_EQ(spCtx->strTrace, std::string("FR1"));  // 失败 → 恢复 → 继续
    exec.Stop();
}

/// @brief OnSettled：兑现与拒绝都触发一次（携带最终结果）。
TEST(Promise_OnSettledCallback)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    // 成功链
    std::shared_ptr<CTestContext> spOk = std::make_shared<CTestContext>();
    std::atomic<int> nOk(0);
    std::atomic<bool> bOkDone(false);
    common::async::CPromise<CTestContext> chainOk =
        exec.NewPromise(spOk, &StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC);
    ASSERT_TRUE(chainOk.OnSettled(
        [&nOk, &bOkDone](common::async::CPromiseResult r)
        {
            nOk.store(r.Code() + 1);  // 成功：0 + 1
            bOkDone.store(true);
        }));
    chainOk.Await();
    while (!bOkDone.load())
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(nOk.load(), 1);  // Code()==kFulfilled

    // 失败链
    std::shared_ptr<CTestContext> spFail = std::make_shared<CTestContext>();
    spFail->nFailCode = common::async::kBusinessBase + 9;
    std::atomic<int> nCode(-1);
    std::atomic<bool> bFailDone(false);
    common::async::CPromise<CTestContext> chainFail = exec.NewPromise(spFail, &StepFail, ASYNC_LOC);
    ASSERT_TRUE(chainFail.OnSettled(
        [&nCode, &bFailDone](common::async::CPromiseResult r)
        {
            nCode.store(r.Code());
            bFailDone.store(true);
        }));
    chainFail.Await();
    while (!bFailDone.load())
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(nCode.load(), common::async::kBusinessBase + 9);
    exec.Stop();
}

// ==================== 注册时机与分叉 ====================

/// @brief 分叉：同一层注册两个 Then，各自独立延续。
TEST(Promise_Fork)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> head = exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC);

    std::atomic<int> nDone(0);
    common::async::CPromise<CTestContext> branchA = head.Then(&StepForkA, ASYNC_LOC);
    common::async::CPromise<CTestContext> branchB = head.Then(&StepForkB, ASYNC_LOC);
    ASSERT_TRUE(branchA.GetContext() == spCtx);  // 三条链（首层 + 两条分支）共用同一上下文实例
    ASSERT_TRUE(branchB.GetContext() == spCtx);
    branchA.OnSettled(
        [&nDone](common::async::CPromiseResult)
        {
            nDone.fetch_add(1);
        });
    branchB.OnSettled(
        [&nDone](common::async::CPromiseResult)
        {
            nDone.fetch_add(1);
        });

    ASSERT_TRUE(branchA.Await().IsFulfilled());
    ASSERT_TRUE(branchB.Await().IsFulfilled());
    while (nDone.load() < 2)
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(spCtx->nValue, 1);  // 首层那一次（两条分支只写各自字段，不动 nValue）
    ASSERT_EQ(spCtx->nForkA, 1);  // 分支 A 执行一次
    ASSERT_EQ(spCtx->nForkB, 1);  // 分支 B 执行一次
    exec.Stop();
}

/// @brief 链完成后再追加层：投递到执行器异步执行（不阻塞调用方）。
TEST(Promise_ThenAfterSettled)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> chain = exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC);
    ASSERT_TRUE(chain.Await().IsFulfilled());  // 首层已完成

    std::atomic<bool> bDone(false);
    common::async::CPromise<CTestContext> tail = chain.Then(&StepAdd10, ASYNC_LOC);
    tail.OnSettled(
        [&bDone](common::async::CPromiseResult)
        {
            bDone.store(true);
        });

    ASSERT_TRUE(tail.Await().IsFulfilled());
    while (!bDone.load())
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(spCtx->nValue, 11);
    exec.Stop();
}

// ==================== 执行器生命周期与线程模型 ====================

/// @brief 未启动执行器起 promise → 立即被拒绝（kStopped），不阻塞。
TEST(Promise_NotStarted)
{
    common::async::CAsyncExecutor exec(2);
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();

    common::async::CPromise<CTestContext> tail =
        exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC);
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Code(), static_cast<int>(common::async::kStopped));
    ASSERT_EQ(spCtx->nSteps, 0);
}

/// @brief Stop 之后起 promise → 被拒绝（kStopped）。
TEST(Promise_AfterStop)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());
    exec.Stop();

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC).Await();
    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Code(), static_cast<int>(common::async::kStopped));
}

/// @brief Stop 之后重新 Start：可继续起链。
TEST(Promise_RestartExecutor)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx1 = std::make_shared<CTestContext>();
    ASSERT_TRUE(exec.NewPromise(spCtx1, &StepAdd1, ASYNC_LOC).Await().IsFulfilled());
    exec.Stop();

    ASSERT_TRUE(exec.Start());  // 重新启动（重建句柄与线程池）
    std::shared_ptr<CTestContext> spCtx2 = std::make_shared<CTestContext>();
    ASSERT_TRUE(exec.NewPromise(spCtx2, &StepAdd1, ASYNC_LOC).Await().IsFulfilled());
    ASSERT_EQ(spCtx2->nValue, 1);
    exec.Stop();
}

/// @brief 层在工作线程上执行（不在起链线程）。
TEST(Promise_WorkerThread)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    const std::thread::id mainId = std::this_thread::get_id();
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    ASSERT_TRUE(exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC).Await().IsFulfilled());
    ASSERT_TRUE(spCtx->workerId != mainId);
    exec.Stop();
}

/// @brief 执行器析构后，已起的链仍安全完成（句柄保活线程池）。
TEST(Promise_LifetimeAfterDestroy)
{
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> tail;
    {
        common::async::CAsyncExecutor exec(2);
        ASSERT_TRUE(exec.Start());
        tail = exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC);
        exec.Stop();  // 等待已投递任务完成
    }  // 执行器析构

    const common::async::CPromiseResult r = tail.Await();
    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);
}

/// @brief 多线程等待同一链（并发 Get）。
TEST(Promise_ConcurrentAwait)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> tail =
        exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC);

    std::atomic<int> nOk(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
    {
        threads.push_back(std::thread(
            [&tail, &nOk]()
            {
                if (tail.Await().IsFulfilled())
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
TEST(Promise_ParallelPromises)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::atomic<int> nActive(0);
    std::atomic<int> nPeak(0);
    const int kChains = 8;

    std::vector<common::async::CPromise<CTestContext> > chains;
    for (int i = 0; i < kChains; ++i)
    {
        std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
        common::async::CPromise<CTestContext> chain = exec.NewPromise(
            spCtx,
            [&nActive, &nPeak](common::async::CPromiseResult /*upStep*/, const std::shared_ptr<CTestContext>& /*spCtx*/)
            {
                const int nNow = nActive.fetch_add(1) + 1;
                int nCur = nPeak.load();
                while (nCur < nNow && !nPeak.compare_exchange_weak(nCur, nNow))
                {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                nActive.fetch_sub(1);
                return common::async::CPromiseResult::Resolve();
            },
            ASYNC_LOC);
        chains.push_back(chain);
    }

    for (size_t i = 0; i < chains.size(); ++i)
    {
        ASSERT_TRUE(chains[i].Await().IsFulfilled());
    }
    ASSERT_TRUE(nPeak.load() >= 2);  // 4 线程下多条链应并行（而非串行）
    exec.Stop();
}

/// @brief 深链：层数超过内联深度上限仍正确执行（超限改投递，防爆栈）。
TEST(Promise_DeepChain)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    const int kLayers = 300;
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> tail = exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC);
    for (int i = 1; i < kLayers; ++i)
    {
        tail = tail.Then(&StepAdd1, ASYNC_LOC);
    }
    const common::async::CPromiseResult r = tail.Await();

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nSteps, kLayers);
    ASSERT_EQ(spCtx->nValue, kLayers);
    exec.Stop();
}

/// @brief 压力：大量链 × 多层，全部完成且计数精确。
TEST(Promise_Stress)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    const int kChains = 400;
    std::atomic<long> nSteps(0);
    std::vector<common::async::CPromise<CTestContext> > tails;
    tails.reserve(kChains);
    for (int i = 0; i < kChains; ++i)
    {
        std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
        auto step = [&nSteps](common::async::CPromiseResult upStep, const std::shared_ptr<CTestContext>& sp)
        {
            if (upStep.IsRejected())
            {
                return upStep;
            }
            ++sp->nSteps;
            nSteps.fetch_add(1);
            return common::async::CPromiseResult::Resolve();
        };
        tails.push_back(exec.NewPromise(spCtx, step, ASYNC_LOC).Then(step, ASYNC_LOC).Then(step, ASYNC_LOC));
    }

    int nOk = 0;
    for (size_t i = 0; i < tails.size(); ++i)
    {
        if (tails[i].Await().IsFulfilled())
        {
            ++nOk;
        }
    }
    ASSERT_EQ(nOk, kChains);
    ASSERT_EQ(nSteps.load(), static_cast<long>(kChains) * 3);
    exec.Stop();
}

/// @brief Post：未启动 / 已停止时不接受；启动后任务在工作线程执行。
TEST(Promise_PostBehavior)
{
    common::async::CAsyncExecutor exec(2);
    std::atomic<int> nDone(0);
    ASSERT_TRUE(!exec.Post(
        [&nDone]()
        {
            nDone.fetch_add(1);
        }));  // 未启动

    ASSERT_TRUE(exec.Start());
    const std::thread::id mainId = std::this_thread::get_id();
    std::thread::id workerId;
    ASSERT_TRUE(exec.Post(
        [&nDone, &workerId, mainId]()
        {
            workerId = std::this_thread::get_id();
            (void)mainId;
            nDone.fetch_add(1);
        }));
    exec.Stop();  // 等待任务完成
    ASSERT_EQ(nDone.load(), 1);
    ASSERT_TRUE(workerId != mainId);

    ASSERT_TRUE(!exec.Post(
        [&nDone]()
        {
            nDone.fetch_add(1);
        }));  // 已停止
    ASSERT_EQ(nDone.load(), 1);
}

// ==================== 协程 ====================

/// 协程：顺序 await 两条子链（协程与子链共享同一上下文）。
class CSequentialCoro : public common::async::CCoroutine<CTestContext>
{
public:
    using common::async::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepAdd1));
        CO_AWAIT(NewPromise(&StepAdd10));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：并行 await 两条子链。
class CParallelCoro : public common::async::CCoroutine<CTestContext>
{
public:
    using common::async::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL(NewPromise(&StepAdd1), NewPromise(&StepAdd10));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：await 到失败 → 终止（失败码透传，后续 await 不执行）。
class CFailCoro : public common::async::CCoroutine<CTestContext>
{
public:
    using common::async::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepFail));
        CO_AWAIT(NewPromise(&StepShouldNotRun));  // 不执行
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程：显式以失败结果结束（CO_RETURN）。
class CReturnFailCoro : public common::async::CCoroutine<CTestContext>
{
public:
    using common::async::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepAdd1));
        CO_RETURN(common::async::CPromiseResult::Reject(common::async::kBusinessBase + 11));
        CO_END();
    }
};

/// 子协程：await 一条子链。
class CChildCoro : public common::async::CCoroutine<CTestContext>
{
public:
    using common::async::CCoroutine<CTestContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepAdd10));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 父协程：await 子协程（嵌套）。
class CParentCoro : public common::async::CCoroutine<CTestContext>
{
public:
    explicit CParentCoro(const std::shared_ptr<CTestContext>& spCtx, common::async::CAsyncExecutor* pExec)
        : common::async::CCoroutine<CTestContext>(spCtx), m_pExec(pExec), m_pChild()
    {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepAdd1));
        m_pChild = m_pExec->CoStart<CChildCoro>(GetContext());  // 跨 await 的变量须为成员
        CO_AWAIT(m_pChild->AsPromise());
        CO_RETURN_VOID();
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    std::shared_ptr<CChildCoro> m_pChild;
};

/// @brief 协程顺序 await：子链共用协程上下文，最终结果为成功。
TEST(Coro_Sequential)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CSequentialCoro> pCoro = exec.CoStart<CSequentialCoro>(spCtx);

    ASSERT_TRUE(pCoro->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);                   // 1 + 10
    ASSERT_EQ(spCtx->strTrace, std::string("12"));  // 顺序确定
    ASSERT_TRUE(pCoro->GetContext() == spCtx);      // 协程与子链共享同一上下文
    exec.Stop();
}

/// @brief 协程并行 await（CO_AWAIT_ALL）。
TEST(Coro_Parallel)
{
    common::async::CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CParallelCoro> pCoro = exec.CoStart<CParallelCoro>(spCtx);

    ASSERT_TRUE(pCoro->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);  // 1 + 10（并行，顺序不定）
    exec.Stop();
}

/// @brief 协程 await 到失败 → 协程终止，失败码透传，后续 await 不执行。
TEST(Coro_AwaitRejected)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->nFailCode = common::async::kBusinessBase + 4;
    std::shared_ptr<CFailCoro> pCoro = exec.CoStart<CFailCoro>(spCtx);

    const common::async::CPromiseResult r = pCoro->Await();
    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Code(), common::async::kBusinessBase + 4);
    ASSERT_EQ(spCtx->strTrace, std::string("F"));  // 后续层未执行
    exec.Stop();
}

/// @brief 协程显式以失败结束（CO_RETURN(Failed(...))）。
TEST(Coro_ReturnRejected)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CReturnFailCoro> pCoro = exec.CoStart<CReturnFailCoro>(spCtx);

    const common::async::CPromiseResult r = pCoro->Await();
    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Code(), common::async::kBusinessBase + 11);
    ASSERT_EQ(spCtx->nValue, 1);  // await 的子链已执行
    exec.Stop();
}

/// @brief 嵌套：协程 await 子协程（AsPromise），共用同一上下文。
TEST(Coro_Nested)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CParentCoro> pCoro = exec.CoStart<CParentCoro>(spCtx, &exec);

    ASSERT_TRUE(pCoro->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);                   // 父 1 + 子 10
    ASSERT_EQ(spCtx->strTrace, std::string("12"));  // 顺序确定
    exec.Stop();
}

/// @brief 未启动执行器起协程 → 立即被拒绝（kStopped），Await 不阻塞。
TEST(Coro_NotStarted)
{
    common::async::CAsyncExecutor exec(2);
    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CSequentialCoro> pCoro = exec.CoStart<CSequentialCoro>(spCtx);

    const common::async::CPromiseResult r = pCoro->Await();
    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Code(), static_cast<int>(common::async::kStopped));
}

/// @brief 协程可重复启动（每次 CoStart 都是一个新对象，复用同一上下文）。
TEST(Coro_Restart)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CSequentialCoro> pCoro = exec.CoStart<CSequentialCoro>(spCtx);
    ASSERT_TRUE(pCoro->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);

    // 再次启动：内部会先复位状态再投递首次执行（对外只有 CoStart 一条启动路径）。
    std::shared_ptr<CSequentialCoro> pCoroAgain = exec.CoStart<CSequentialCoro>(spCtx);
    ASSERT_TRUE(pCoroAgain->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 22);
    exec.Stop();
}

/// @brief 协程作为可等待 promise 注册 settled 通知（AsPromise + OnSettled）。
TEST(Coro_OnSettledCallback)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CSequentialCoro> pCoro = exec.CoStart<CSequentialCoro>(spCtx);

    std::atomic<int> nCode(-1);
    ASSERT_TRUE(pCoro->AsPromise().OnSettled(
        [&nCode](common::async::CPromiseResult r)
        {
            nCode.store(r.Code());
        }));
    while (nCode.load() < 0)
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(nCode.load(), static_cast<int>(common::async::kFulfilled));
    exec.Stop();
}

/// @brief then 与 catch 互补：then 在兑现时执行、被拒绝时跳过；catch 相反。
TEST(Promise_ThenAndCatchComplement)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    // 兑现：then 执行、catch 不执行。
    std::shared_ptr<CTestContext> spOk = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> tailOk =
        exec.NewPromise(spOk, &StepAdd1, ASYNC_LOC).Then(&StepAdd10, ASYNC_LOC).Catch(&StepCatchObserve, ASYNC_LOC);
    ASSERT_TRUE(tailOk.Await().IsFulfilled());
    ASSERT_EQ(spOk->nValue, 11);     // then 路径已执行
    ASSERT_EQ(spOk->nCatchRuns, 0);  // catch 未执行（上一层兑现）
    ASSERT_EQ(spOk->strTrace, std::string("12"));

    // 拒绝：catch 执行（upResult 携带拒绝结果）、后续 then 跳过。
    std::shared_ptr<CTestContext> spBad = std::make_shared<CTestContext>();
    spBad->nFailCode = common::async::kBusinessBase + 21;
    common::async::CPromise<CTestContext> tailBad = exec.NewPromise(spBad, &StepFail, ASYNC_LOC)
                                                        .Then(&StepShouldNotRun, ASYNC_LOC)
                                                        .Catch(&StepCatchObserve, ASYNC_LOC)
                                                        .Then(&StepShouldNotRun, ASYNC_LOC);
    const common::async::CPromiseResult r = tailBad.Await();
    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Code(), common::async::kBusinessBase + 21);
    ASSERT_EQ(spBad->nCatchRuns, 1);                // catch 执行了
    ASSERT_EQ(spBad->nSeenFailed, 1);               // 真的看到了上一层的拒绝
    ASSERT_EQ(spBad->strTrace, std::string("FA"));  // 拒绝层 + catch（无 then 层痕迹）
    exec.Stop();
}

/// @brief finally：无论兑现还是拒绝都执行，且不改结果（忽略处理器返回值）。
TEST(Promise_FinallyKeepsResult)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    // 拒绍路径：finally 执行，但结果仍为拒绍（不像 catch 那样能吞掉拒绍）。
    std::shared_ptr<CTestContext> spBad = std::make_shared<CTestContext>();
    spBad->nFailCode = common::async::kBusinessBase + 31;
    const common::async::CPromiseResult rBad = exec.NewPromise(spBad, &StepFail, ASYNC_LOC)
                                                   .Finally(&StepCatchRecover, ASYNC_LOC)  // 返回 Resolve() 但被忽略
                                                   .Await();
    ASSERT_TRUE(rBad.IsRejected());
    ASSERT_EQ(rBad.Code(), common::async::kBusinessBase + 31);
    ASSERT_EQ(spBad->nCatchRuns, 1);  // finally 已执行

    // 兑现路径：finally 也执行，结果仍为兑现。
    std::shared_ptr<CTestContext> spOk = std::make_shared<CTestContext>();
    const common::async::CPromiseResult rOk =
        exec.NewPromise(spOk, &StepAdd1, ASYNC_LOC).Finally(&StepCatchRecover, ASYNC_LOC).Await();
    ASSERT_TRUE(rOk.IsFulfilled());
    ASSERT_EQ(spOk->nCatchRuns, 1);
    ASSERT_EQ(spOk->nValue, 1);
    exec.Stop();
}

/// @brief 起链即投递首层：exec.NewPromise(spCtx, 首层) 等价 JS new Promise(executor)。
TEST(Promise_NewPromiseStarts)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    common::async::CPromise<CTestContext> p = exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC);  // 起链即投递首层
    ASSERT_EQ(p.Await().Code(), static_cast<int>(common::async::kFulfilled));
    ASSERT_EQ(spCtx->nValue, 1);

    common::async::CPromise<CTestContext> p2 = p.Then(&StepAdd10, ASYNC_LOC);
    ASSERT_TRUE(p2.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);
    exec.Stop();
}

// ==================== 嵌套：跨上下文 await ====================

/// @brief 子流程上下文（与 CTestContext **不同** —— 验证跨上下文 await）。
struct COtherContext
{
    int nRows;  ///< 子流程查询到的行数。

    COtherContext() : nRows(0)
    {}
};

/// 处理器（子流程）：置 nRows = 3。
static common::async::CPromiseResult StepQueryRows(
    common::async::CPromiseResult upResult, const std::shared_ptr<COtherContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    spCtx->nRows = 3;
    return common::async::CPromiseResult::Resolve();
}

/// 协程：await 另一套上下文的子流程（跨上下文嵌套）。
class CCrossContextCoro : public common::async::CCoroutine<CTestContext>
{
public:
    explicit CCrossContextCoro(const std::shared_ptr<CTestContext>& spCtx, common::async::CAsyncExecutor* pExec)
        : common::async::CCoroutine<CTestContext>(spCtx), m_pExec(pExec), m_spOther()
    {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepAdd1));                                      // 同上下文子 promise
        m_spOther = std::make_shared<COtherContext>();                        // 跨 await → 成员变量
        CO_AWAIT(m_pExec->NewPromise(m_spOther, &StepQueryRows, ASYNC_LOC));  // 跨上下文 await
        GetContext()->nValue += m_spOther->nRows;                             // 恢复后并入
        CO_RETURN_VOID();
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    std::shared_ptr<COtherContext> m_spOther;
};

/// @brief 跨上下文嵌套：协程 await 另一套 TContext 的子流程。
TEST(Coro_AwaitOtherContext)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CCrossContextCoro> pCoro = exec.CoStart<CCrossContextCoro>(spCtx, &exec);

    ASSERT_TRUE(pCoro->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 4);  // StepAdd1（1）+ 子流程 3 行
    ASSERT_EQ(spCtx->nSteps, 1);
    exec.Stop();
}

/// 协程：并行 await「同上下文 + 两套别的上下文」的混合列表。
class CMixedParallelCoro : public common::async::CCoroutine<CTestContext>
{
public:
    explicit CMixedParallelCoro(const std::shared_ptr<CTestContext>& spCtx, common::async::CAsyncExecutor* pExec)
        : common::async::CCoroutine<CTestContext>(spCtx), m_pExec(pExec), m_spOtherA(), m_spOtherB()
    {}

    void Run() override
    {
        CO_BEGIN();
        m_spOtherA = std::make_shared<COtherContext>();  // 跨 await → 成员变量
        m_spOtherB = std::make_shared<COtherContext>();
        CO_AWAIT_ALL(NewPromise(&StepAdd1),                              // 同上下文
            m_pExec->NewPromise(m_spOtherA, &StepQueryRows, ASYNC_LOC),  // 另一套上下文
            m_pExec->NewPromise(m_spOtherB, &StepQueryRows, ASYNC_LOC));
        GetContext()->nValue += m_spOtherA->nRows + m_spOtherB->nRows;
        CO_RETURN_VOID();
        CO_END();
    }

private:
    common::async::CAsyncExecutor* m_pExec;
    std::shared_ptr<COtherContext> m_spOtherA;
    std::shared_ptr<COtherContext> m_spOtherB;
};

/// @brief 并行 await 混合上下文列表（CO_AWAIT_ALL 支持每条 promise 不同类型）。
TEST(Coro_ParallelAwaitMixedContext)
{
    common::async::CAsyncExecutor exec(3);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<CMixedParallelCoro> pCoro = exec.CoStart<CMixedParallelCoro>(spCtx, &exec);

    ASSERT_TRUE(pCoro->Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 7);  // 1 + 3 + 3
    exec.Stop();
}

// ==================== 纯异步组合：new Promise + then-promise（无协程、零阻塞） ====================

/// 别的模块的错误码（业务码从 kBusinessBase 起取）。
enum BridgeCode
{
    kOtherModuleFailed = common::async::kBusinessBase + 1  ///< 别的模块（子流程）失败。
};

/// 处理器（子流程）：模拟别的模块失败。
static common::async::CPromiseResult StepQueryRowsFail(
    common::async::CPromiseResult upResult, const std::shared_ptr<COtherContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    spCtx->nRows = -1;
    return common::async::CPromiseResult::Reject(kOtherModuleFailed);
}

/// @brief 跨模块组合（成功路径）：new Promise 桥接别的 promise + then-promise 等它。
///
/// 本流程：本模块层 → 桥接（别的模块的 promise，另一套上下文）→ 汇总 → 本模块层；
/// 全程回调驱动、不占工作线程（exec 只有 2 个 worker 也能跑）。
TEST(Promise_BridgeForeignPromise)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    spCtx->nValue = 100;
    std::shared_ptr<COtherContext> spOther = std::make_shared<COtherContext>();

    // 别的模块（另一套 TContext）的异步 promise。
    std::shared_ptr<common::async::CPromise<COtherContext> > spForeign(
        new common::async::CPromise<COtherContext>(exec.NewPromise(spOther, &StepQueryRows, ASYNC_LOC)));

    common::async::CPromise<CTestContext> pTail =
        exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC)  // 本模块层
            .ThenPromise(
                [&exec, spForeign](const std::shared_ptr<CTestContext>& spCtxSelf)
                {
                    // new Promise：由「别的模块」的完成回调兑现 / 拒绝本 promise（非阻塞桥接）。
                    return exec.NewPromise(
                        spCtxSelf,

                        [spForeign, spCtxSelf](const common::async::CPromise<CTestContext>::ResolveFn& fnResolve,
                            const common::async::CPromise<CTestContext>::RejectFn& fnReject)
                        {
                            spForeign->OnSettled(
                                [spForeign, spCtxSelf, fnResolve, fnReject](common::async::CPromiseResult result)
                                {
                                    if (result.IsRejected())
                                    {
                                        fnReject(result.Code());  // 拒绝：本流程后续 then 层不执行。
                                        return;
                                    }
                                    spCtxSelf->nValue += spForeign->GetContext()->nRows;  // 汇总别的模块的数据。
                                    fnResolve();
                                });
                        },
                        ASYNC_LOC);
                },
                ASYNC_LOC)
            .Then(&StepAdd10, ASYNC_LOC);  // 子 promise 完成后继续本模块层

    ASSERT_TRUE(pTail.Await().IsFulfilled());
    ASSERT_EQ(spOther->nRows, 3);
    ASSERT_EQ(spCtx->nValue, 114);  // 100 + 1（本模块层）+ 3（子流程）+ 10（本模块层）
    ASSERT_EQ(spCtx->nSteps, 2);
    exec.Stop();
}

/// @brief 跨模块组合（拒绝路径）：子 promise 被拒绝 → 本流程 then 层不执行、Catch 仍可处理。
TEST(Promise_BridgeForeignRejected)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTestContext> spCtx = std::make_shared<CTestContext>();
    std::shared_ptr<COtherContext> spOther = std::make_shared<COtherContext>();

    std::shared_ptr<common::async::CPromise<COtherContext> > spForeign(
        new common::async::CPromise<COtherContext>(exec.NewPromise(spOther, &StepQueryRowsFail, ASYNC_LOC)));

    common::async::CPromise<CTestContext> pTail =
        exec.NewPromise(spCtx, &StepAdd1, ASYNC_LOC)
            .ThenPromise(
                [&exec, spForeign](const std::shared_ptr<CTestContext>& spCtxSelf)
                {
                    return exec.NewPromise(
                        spCtxSelf,

                        [spForeign](const common::async::CPromise<CTestContext>::ResolveFn& fnResolve,
                            const common::async::CPromise<CTestContext>::RejectFn& fnReject)
                        {
                            spForeign->OnSettled(
                                [fnResolve, fnReject](common::async::CPromiseResult result)
                                {
                                    if (result.IsRejected())
                                    {
                                        fnReject(result.Code());
                                        return;
                                    }
                                    fnResolve();
                                });
                        },
                        ASYNC_LOC);
                },
                ASYNC_LOC)
            .Then(&StepShouldNotRun, ASYNC_LOC)    // 子 promise 被拒绝 → 不执行
            .Catch(&StepCatchObserve, ASYNC_LOC);  // catch 仍执行（观察拒绝）

    const common::async::CPromiseResult result = pTail.Await();
    ASSERT_TRUE(result.IsRejected());
    ASSERT_EQ(result.Code(), static_cast<int>(kOtherModuleFailed));  // 拒绝码透传
    ASSERT_EQ(spCtx->nSteps, 1);                                     // 只有桥接前的 StepAdd1 执行
    ASSERT_EQ(spCtx->nSeenFailed, 1);                                // catch 观察到拒绝
    ASSERT_EQ(spCtx->nCatchRuns, 1);
    exec.Stop();
}
