/// @file test_async_combine.cpp
/// 组合器（`WhenAll` / `WhenAllSettled` / `WhenRace` / `WhenAny`）的专项测试。
///
/// 承诺（本文件即验收标准）：
///  - `WhenAll`：全部兑现才兑现；**任一拒绝立即以该拒绝结果收口**（不等其余分支）；
///  - `WhenAllSettled`：全部落定即兑现，**不因任何分支拒绝而失败**（成败由调用方从子句柄读）；
///  - `WhenRace`：首个落定者定结果（**拒绝也算结论**）；
///  - `WhenAny`：首个兑现者定结果（先失败的分支不算结论）；全部拒绝才以首个拒绝结果收口；
///  - 子 promise 可**跨上下文类型 / 跨模块**汇聚（聚合 promise 只关心分支成败）；
///  - 已落定的子 promise 直接计入（绝不永久 pending）；
///  - 空集合：`all` / `allSettled` 立即兑现；`race` / `any` 立即以系统侧失败 `Refused()` 拒绝（不死等）；
///  - 聚合 promise 的后续层跑在**传入的执行器**上；框架不取消落败分支（它们照旧跑完）。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "AsyncTestKit.h"
#include "TestFramework.h"

// ==================== 用例脚手架 ====================

/// 各分支的拒绝文案（拒绝统一用标准异常表达；框架只搬运、不解释）。
static const char* const kBranchRejectA = "分支 A 拒绝";
static const char* const kBranchRejectB = "分支 B 拒绝";
static const char* const kBranchRejectC = "分支 C 拒绝";
static const char* const kBranchRejectD = "分支 D 拒绝";
static const char* const kCalleeRejectText = "被调模块拒绝（组合器用例）";

/// @brief 组合器用例的共享上下文（各分支只写自己的字段）。
struct CCombineCtx
{
    std::shared_ptr<asynctest::CTraceSink> spTrace;  ///< 步骤轨迹（跨线程写入，内部加锁）。
    std::atomic<int> nDone;                          ///< 已完成的分支步骤数。
    std::atomic<int> nDoneAtGather;                  ///< 汇聚层执行**那一刻**看到的已完成分支数。
    std::atomic<int> nGate;                          ///< 手动放行门（0 = 未放行）。
    int nGatherRuns;                                 ///< 汇聚层执行次数（汇聚层自己写）。
    std::thread::id idGather;                        ///< 汇聚层所在线程（只由汇聚层写）。

    CCombineCtx() : nDone(0), nDoneAtGather(0), nGate(0), nGatherRuns(0)
    {}
};

/// @brief 等门放行（固定「谁先落定」的时序用；不依赖 sleep 时长，TSan 下同样确定）。
///
/// @param spCtx 上下文（门在 nGate 上）。
void WaitGate(const std::shared_ptr<CCombineCtx>& spCtx)
{
    while (spCtx->nGate.load() == 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/// @brief 造一个「本分支一步」的处理器：打轨迹、计数，可按需延时 / 等门 / 拒绝。
///
/// @param strTag 轨迹标签（如 "A"）。
/// @param nDelayMs 本步模拟耗时（毫秒）。
/// @param bWaitGate 是否卡在放行门上（门由测试放行 → 时序完全确定）。
/// @param pszRejectText 非 null 则本步以该文案拒绝（拒绝统一用标准异常表达）。
/// @return 本层处理器。
common::async::CPromise<CCombineCtx>::ThenHandler MakeBranchStep(
    const char* strTag, int nDelayMs = 0, bool bWaitGate = false, const char* pszRejectText = nullptr)
{
    return [strTag, nDelayMs, bWaitGate, pszRejectText](
               common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CCombineCtx>& spCtx)
    {
        if (spCtx->spTrace != nullptr)
        {
            spCtx->spTrace->Append(strTag);
        }
        ++spCtx->nDone;
        asynctest::SleepMs(nDelayMs);
        if (bWaitGate)
        {
            WaitGate(spCtx);
        }
        if (pszRejectText != nullptr)
        {
            return common::async::CPromiseResult::Reject(std::runtime_error(pszRejectText));
        }
        return common::async::CPromiseResult::Resolve();
    };
}

/// @brief 汇聚层处理器：记录「此刻已完成的分支数」与所在线程（证明等待语义）。
common::async::CPromiseResult StepGather(common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CCombineCtx>& spCtx)
{
    ++spCtx->nGatherRuns;
    spCtx->idGather = std::this_thread::get_id();
    spCtx->nDoneAtGather.store(spCtx->nDone.load());
    if (spCtx->spTrace != nullptr)
    {
        spCtx->spTrace->Append("G");
    }
    return common::async::CPromiseResult::Resolve();
}

// ==================== WhenAll ====================

TEST(Combine_AllWaitsForEveryBranch)
{
    common::async::CAsyncExecutor execA(1), execB(1), execAgg(1);
    execA.Start();
    execB.Start();
    execAgg.Start();

    std::shared_ptr<CCombineCtx> spCtx = std::make_shared<CCombineCtx>();
    spCtx->spTrace = std::make_shared<asynctest::CTraceSink>();

    const common::async::CPromise<CCombineCtx> pA = execA.NewPromise(spCtx, MakeBranchStep("A", 30), ASYNC_LOC);
    const common::async::CPromise<CCombineCtx> pB = execB.NewPromise(spCtx, MakeBranchStep("B"), ASYNC_LOC);

    const common::async::CPromise<CCombineCtx> pDone = execAgg.WhenAll(spCtx, pA, pB).Then(StepGather, ASYNC_LOC);
    ASSERT_TRUE(pDone.AwaitFor(3000).IsFulfilled());

    ASSERT_EQ(spCtx->nDone.load(), 2);          // 两条分支都跑过
    ASSERT_EQ(spCtx->nDoneAtGather.load(), 2);  // 汇聚层看到的是「全部完成」
    ASSERT_EQ(spCtx->spTrace->Count("G"), 1);   // 汇聚层只跑一次
    ASSERT_EQ(spCtx->spTrace->strTrace.back(), ';');

    execA.Stop();
    execB.Stop();
    execAgg.Stop();
}

TEST(Combine_AllFailFastDoesNotWaitOtherBranches)
{
    common::async::CAsyncExecutor execReject(1), execSlow(1), execAgg(1);
    execReject.Start();
    execSlow.Start();
    execAgg.Start();

    std::shared_ptr<CCombineCtx> spCtx = std::make_shared<CCombineCtx>();

    // A：立即拒绝；B：卡在门上（什么时候跑完由测试决定）。
    const common::async::CPromise<CCombineCtx> pA =
        execReject.NewPromise(spCtx, MakeBranchStep("A", 0, false, kBranchRejectA), ASYNC_LOC);
    const common::async::CPromise<CCombineCtx> pB = execSlow.NewPromise(spCtx, MakeBranchStep("B", 0, true), ASYNC_LOC);

    const common::async::CPromise<CCombineCtx> pAll = execAgg.WhenAll(spCtx, pA, pB);
    const common::async::CPromiseResult resultAll = pAll.AwaitFor(3000);

    ASSERT_TRUE(resultAll.IsRejected());
    ASSERT_EQ(resultAll.Message(), std::string(kBranchRejectA));  // 异常原样收口（框架不解释）
    ASSERT_TRUE(!pB.IsSettled());                                 // 及时失败：没等 B

    spCtx->nGate.store(1);                         // 放行 B
    ASSERT_TRUE(pB.AwaitFor(3000).IsFulfilled());  // 框架不取消分支，B 照旧跑完
    ASSERT_EQ(spCtx->nDone.load(), 2);

    execReject.Stop();
    execSlow.Stop();
    execAgg.Stop();
}

TEST(Combine_AllCountsAlreadySettledChild)
{
    common::async::CAsyncExecutor exec(1), execAgg(1);
    exec.Start();
    execAgg.Start();

    std::shared_ptr<CCombineCtx> spCtx = std::make_shared<CCombineCtx>();

    const common::async::CPromise<CCombineCtx> pA = exec.NewPromise(spCtx, MakeBranchStep("A"), ASYNC_LOC);
    const common::async::CPromise<CCombineCtx> pB = exec.NewPromise(spCtx, MakeBranchStep("B"), ASYNC_LOC);
    ASSERT_TRUE(pA.AwaitFor(3000).IsFulfilled());  // 先落定，再汇聚（走「已 settled」路径）
    ASSERT_TRUE(pB.AwaitFor(3000).IsFulfilled());

    const common::async::CPromise<CCombineCtx> pAll = execAgg.WhenAll(spCtx, pA, pB);
    ASSERT_TRUE(pAll.AwaitFor(3000).IsFulfilled());
    ASSERT_EQ(spCtx->nDone.load(), 2);

    exec.Stop();
    execAgg.Stop();
}

// ==================== WhenAllSettled ====================

TEST(Combine_AllSettledIgnoresRejects)
{
    common::async::CAsyncExecutor execA(1), execB(1), execAgg(1);
    execA.Start();
    execB.Start();
    execAgg.Start();

    std::shared_ptr<CCombineCtx> spCtx = std::make_shared<CCombineCtx>();
    spCtx->spTrace = std::make_shared<asynctest::CTraceSink>();

    const common::async::CPromise<CCombineCtx> pA =
        execA.NewPromise(spCtx, MakeBranchStep("A", 0, false, kBranchRejectA), ASYNC_LOC);
    const common::async::CPromise<CCombineCtx> pB = execB.NewPromise(spCtx, MakeBranchStep("B"), ASYNC_LOC);

    const common::async::CPromise<CCombineCtx> pDone = execAgg.WhenAllSettled(spCtx, pA, pB).Then(StepGather, ASYNC_LOC);
    ASSERT_TRUE(pDone.AwaitFor(3000).IsFulfilled());  // 有分支拒绝，但聚合仍然兑现

    ASSERT_EQ(spCtx->nDone.load(), 2);
    ASSERT_EQ(spCtx->nDoneAtGather.load(), 2);

    // 各分支的成败由调用方从子句柄读（此处都已落定 → Await 立即返回，不阻塞）。
    const common::async::CPromiseResult resultA = pA.Await();
    ASSERT_TRUE(resultA.IsRejected());
    ASSERT_EQ(resultA.Message(), std::string(kBranchRejectA));
    ASSERT_TRUE(pB.Await().IsFulfilled());

    execA.Stop();
    execB.Stop();
    execAgg.Stop();
}

// ==================== WhenRace ====================

TEST(Combine_RaceFirstSettledWins)
{
    common::async::CAsyncExecutor execSlow(1), execFast(1), execAgg(1);
    execSlow.Start();
    execFast.Start();
    execAgg.Start();

    std::shared_ptr<CCombineCtx> spCtx = std::make_shared<CCombineCtx>();

    // A：卡在门上（慢）；B：立即拒绝 → race 以「首个落定」的 B 收口（拒绝也算结论）。
    const common::async::CPromise<CCombineCtx> pA = execSlow.NewPromise(spCtx, MakeBranchStep("A", 0, true), ASYNC_LOC);
    const common::async::CPromise<CCombineCtx> pB =
        execFast.NewPromise(spCtx, MakeBranchStep("B", 0, false, kBranchRejectB), ASYNC_LOC);

    const common::async::CPromiseResult resultRace = execAgg.WhenRace(spCtx, pA, pB).AwaitFor(3000);
    ASSERT_TRUE(resultRace.IsRejected());
    ASSERT_EQ(resultRace.Message(), std::string(kBranchRejectB));

    spCtx->nGate.store(1);  // 放行 A（不取消分支）
    ASSERT_TRUE(pA.AwaitFor(3000).IsFulfilled());

    execSlow.Stop();
    execFast.Stop();
    execAgg.Stop();
}

TEST(Combine_RaceFirstFulfilledWins)
{
    common::async::CAsyncExecutor execSlow(1), execFast(1), execAgg(1);
    execSlow.Start();
    execFast.Start();
    execAgg.Start();

    std::shared_ptr<CCombineCtx> spCtx = std::make_shared<CCombineCtx>();

    const common::async::CPromise<CCombineCtx> pA = execSlow.NewPromise(spCtx, MakeBranchStep("A", 0, true), ASYNC_LOC);
    const common::async::CPromise<CCombineCtx> pB = execFast.NewPromise(spCtx, MakeBranchStep("B"), ASYNC_LOC);

    ASSERT_TRUE(execAgg.WhenRace(spCtx, pA, pB).AwaitFor(3000).IsFulfilled());

    spCtx->nGate.store(1);
    ASSERT_TRUE(pA.AwaitFor(3000).IsFulfilled());

    execSlow.Stop();
    execFast.Stop();
    execAgg.Stop();
}

// ==================== WhenAny ====================

TEST(Combine_AnyIgnoresEarlierReject)
{
    common::async::CAsyncExecutor execReject(1), execSlow(1), execAgg(1);
    execReject.Start();
    execSlow.Start();
    execAgg.Start();

    std::shared_ptr<CCombineCtx> spCtx = std::make_shared<CCombineCtx>();

    // A：立即拒绝（不算结论）；B：稍后兑现 → any 以 B 的兑现收口（与 race 的关键差别）。
    const common::async::CPromise<CCombineCtx> pA =
        execReject.NewPromise(spCtx, MakeBranchStep("A", 0, false, kBranchRejectA), ASYNC_LOC);
    const common::async::CPromise<CCombineCtx> pB = execSlow.NewPromise(spCtx, MakeBranchStep("B", 30), ASYNC_LOC);

    const common::async::CPromiseResult resultAny = execAgg.WhenAny(spCtx, pA, pB).AwaitFor(3000);
    ASSERT_TRUE(resultAny.IsFulfilled());
    ASSERT_EQ(spCtx->nDone.load(), 2);  // 两条分支都跑过（拒绝的那条不被取消）

    execReject.Stop();
    execSlow.Stop();
    execAgg.Stop();
}

TEST(Combine_AnyAllRejectedUsesFirstCode)
{
    common::async::CAsyncExecutor exec(1), execAgg(1);
    exec.Start();
    execAgg.Start();

    std::shared_ptr<CCombineCtx> spCtx = std::make_shared<CCombineCtx>();

    // 先让两条分支都落定，再汇聚：登记顺序即「先到」顺序（同执行器 FIFO，确定可断言）。
    const common::async::CPromise<CCombineCtx> pA =
        exec.NewPromise(spCtx, MakeBranchStep("A", 0, false, kBranchRejectA), ASYNC_LOC);
    const common::async::CPromise<CCombineCtx> pB =
        exec.NewPromise(spCtx, MakeBranchStep("B", 0, false, kBranchRejectB), ASYNC_LOC);
    ASSERT_TRUE(pA.AwaitFor(3000).IsRejected());
    ASSERT_TRUE(pB.AwaitFor(3000).IsRejected());

    const common::async::CPromiseResult resultAny = execAgg.WhenAny(spCtx, pB, pA).AwaitFor(3000);
    ASSERT_TRUE(resultAny.IsRejected());
    ASSERT_EQ(resultAny.Message(), std::string(kBranchRejectB));  // 首个拒绝 = 先登记的那个

    // 并发场景下「首个拒绝」取决于实际落定顺序 → 只断言「是其中之一」。
    const common::async::CPromise<CCombineCtx> pC =
        exec.NewPromise(spCtx, MakeBranchStep("C", 0, false, kBranchRejectC), ASYNC_LOC);
    const common::async::CPromise<CCombineCtx> pD =
        exec.NewPromise(spCtx, MakeBranchStep("D", 0, false, kBranchRejectD), ASYNC_LOC);
    const common::async::CPromiseResult resultAny2 = execAgg.WhenAny(spCtx, pC, pD).AwaitFor(3000);
    ASSERT_TRUE(resultAny2.IsRejected());
    ASSERT_TRUE(resultAny2.Message() == kBranchRejectC || resultAny2.Message() == kBranchRejectD);

    exec.Stop();
    execAgg.Stop();
}

// ==================== 空集合 / 跨上下文 / 线程归属 ====================

TEST(Combine_EmptyChildSetSettlesImmediately)
{
    common::async::CAsyncExecutor execAgg(1);
    execAgg.Start();

    std::shared_ptr<CCombineCtx> spCtx = std::make_shared<CCombineCtx>();

    // 没有要等的东西：all / allSettled 视为成功。
    ASSERT_TRUE(execAgg.WhenAll(spCtx).AwaitFor(3000).IsFulfilled());
    ASSERT_TRUE(execAgg.WhenAllSettled(spCtx).AwaitFor(3000).IsFulfilled());

    // race / any 不可能有结果 → 立即以框架侧拒绝收口（绝不永久 pending）。
    const common::async::CPromiseResult resultRace = execAgg.WhenRace(spCtx).AwaitFor(3000);
    ASSERT_TRUE(resultRace.IsRejected());
    ASSERT_TRUE(asynctest::IsUnspecifiedFailure(resultRace));  // 框架侧拒绝（预建固定文案）
    ASSERT_EQ(resultRace.Message(), std::string("未指定原因"));
    ASSERT_TRUE(execAgg.WhenAny(spCtx).AwaitFor(3000).IsRejected());

    execAgg.Stop();
}

TEST(Combine_CrossContextAndCrossModuleBranches)
{
    asynctest::CCalleeModule callee;  // 被调模块：自持 1 线程执行器
    common::async::CAsyncExecutor execLocal(1), execAgg(1);
    execLocal.Start();
    execAgg.Start();

    std::shared_ptr<CCombineCtx> spCtx = std::make_shared<CCombineCtx>();
    std::shared_ptr<asynctest::CCalleeCtx> spCallee = std::make_shared<asynctest::CCalleeCtx>();
    spCallee->nDelayMs = 10;
    spCallee->spTrace = std::make_shared<asynctest::CTraceSink>();

    // 两条分支的上下文类型不同（被调模块的 CCalleeCtx + 本模块的 CCombineCtx）也能汇到一起。
    const common::async::CPromise<asynctest::CCalleeCtx> pStock = callee.QueryStockAsync(spCallee);
    const common::async::CPromise<CCombineCtx> pLocal = execLocal.NewPromise(spCtx, MakeBranchStep("A"), ASYNC_LOC);

    const common::async::CPromise<CCombineCtx> pAll = execAgg.WhenAll(spCtx, pStock, pLocal);
    ASSERT_TRUE(pAll.AwaitFor(3000).IsFulfilled());
    ASSERT_EQ(spCallee->nAvail, 5);  // 跨模块分支确实跑完了（数据在它自己的上下文里）
    ASSERT_EQ(spCtx->nDone.load(), 1);

    // 跨模块分支拒绝 → 聚合以对方的拒绝（异常）收口。
    spCallee->bReject = true;
    spCallee->strRejectText = kCalleeRejectText;
    const common::async::CPromise<asynctest::CCalleeCtx> pStockFail = callee.QueryStockAsync(spCallee);
    const common::async::CPromise<CCombineCtx> pLocal2 = execLocal.NewPromise(spCtx, MakeBranchStep("B"), ASYNC_LOC);
    const common::async::CPromiseResult resultAll = execAgg.WhenAll(spCtx, pStockFail, pLocal2).AwaitFor(3000);
    ASSERT_TRUE(resultAll.IsRejected());
    ASSERT_EQ(resultAll.Message(), std::string(kCalleeRejectText));

    execLocal.Stop();
    execAgg.Stop();
}

TEST(Combine_AggregateLayersRunOnItsExecutor)
{
    common::async::CAsyncExecutor execBranch(1), execAgg(1);
    execBranch.Start();
    execAgg.Start();

    std::shared_ptr<CCombineCtx> spCtx = std::make_shared<CCombineCtx>();
    spCtx->spTrace = std::make_shared<asynctest::CTraceSink>();  // 分支步骤的线程从轨迹里读

    const common::async::CPromise<CCombineCtx> pA = execBranch.NewPromise(spCtx, MakeBranchStep("A"), ASYNC_LOC);
    const common::async::CPromise<CCombineCtx> pB = execBranch.NewPromise(spCtx, MakeBranchStep("B"), ASYNC_LOC);

    common::async::CPromise<CCombineCtx> pDone = execAgg.WhenAll(spCtx, pA, pB).Then(StepGather, ASYNC_LOC);
    ASSERT_TRUE(pDone.AwaitFor(3000).IsFulfilled());

    ASSERT_TRUE(spCtx->idGather != std::this_thread::get_id());  // 不在测试主线程上
    // 在聚合执行器上，不是分支执行器上（分支步骤的线程从轨迹里读：同一步可能被多条分支并发写）
    ASSERT_TRUE(spCtx->idGather != spCtx->spTrace->ThreadOf("A"));
    ASSERT_EQ(spCtx->nGatherRuns, 1);

    // 聚合链上再挂一层：线程不变（仍是聚合执行器），说明聚合 promise 是一条普通链。
    const std::thread::id idGather = spCtx->idGather;
    pDone = pDone.Then(StepGather, ASYNC_LOC);
    ASSERT_TRUE(pDone.AwaitFor(3000).IsFulfilled());
    ASSERT_TRUE(spCtx->idGather == idGather);
    ASSERT_EQ(spCtx->nGatherRuns, 2);

    execBranch.Stop();
    execAgg.Stop();
}

TEST(Combine_ManyBranchesConcurrent)
{
    common::async::CAsyncExecutor execBranches(4), execAgg(2);
    execBranches.Start();
    execAgg.Start();

    for (int nRound = 0; nRound < 10; ++nRound)
    {
        std::shared_ptr<CCombineCtx> spCtx = std::make_shared<CCombineCtx>();
        std::vector<common::async::CPromise<CCombineCtx> > vecBranches;
        for (int i = 0; i < 32; ++i)
        {
            vecBranches.push_back(execBranches.NewPromise(spCtx, MakeBranchStep("A"), ASYNC_LOC));
        }

        // 并发分支多 → 聚合计数必须在锁内判（否则会漏唤醒 / 提前收口）。
        // 同时验证「标量 + 列表混用」：vecBranches 之外再挂一条单句柄分支。
        const common::async::CPromise<CCombineCtx> pExtra = execBranches.NewPromise(spCtx, MakeBranchStep("X"), ASYNC_LOC);
        const common::async::CPromise<CCombineCtx> pAll = execAgg.WhenAll(spCtx, vecBranches, pExtra);
        ASSERT_TRUE(pAll.AwaitFor(5000).IsFulfilled());
        ASSERT_EQ(spCtx->nDone.load(), 33);
    }

    execBranches.Stop();
    execAgg.Stop();
}
