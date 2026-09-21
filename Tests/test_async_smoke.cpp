/// @file test_async_smoke.cpp
/// 异步 promise 冒烟测试：把框架对外承诺的各种用法各跑一遍，确认行为正确。
///
/// 覆盖（与 docs/common/async-usage.md、docs/common/async-vs-js.md 对应）：
///  - 一条链里混用：具名处理器 / lambda / ThenPromise（内层链）/ 旁支 / catch / finally；
///  - then 失败即停、catch 恢复或透传、finally 不改结果；
///  - ThenPromise 等子 promise（flatten）、内层拒绝（异常）沿外层链透传；
///  - `exec.NewPromise(spCtx, fnStarter)`（由外部回调 settle）：resolve / reject / 起链回调抛异常 / 空回调；
///  - 跨模块桥接：两个模块各持执行器，只通过 promise 交接；
///  - 处理器抛异常 → 异常原样成为本层拒绝（动态类型与 what() 都保住），finally / OnSettled 仍执行；
///  - OnSettled 旁路通知：不改结果、可多次登记；
///  - Catch / Finally 紧挂在链首之后（起点已兑现 → Catch 不执行）；
///  - 单线程执行器下嵌套 + 旁支全部完成（证明「不占 worker、不阻塞」）；
///  - 执行器未启动 → 框架侧拒绝「执行器已停」；Await 取最终结果；上下文为全链同一实例。

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "AsyncTestKit.h"
#include "TestFramework.h"

// ==================== 冒烟测试用的上下文与层函数 ====================

/// @brief 冒烟测试的共享上下文（整条链共用同一实例）。
struct CSmokeCtx
{
    int nValue;                ///< 逐层累加。
    int nSteps;                ///< 已执行的层数。
    int nCatchRuns;            ///< catch 层执行次数。
    int nFinallyRuns;          ///< finally 层执行次数。
    std::string strFailText;   ///< 拒绝原因（异常描述；空 = 不失败）。
    std::atomic<int> nSide;    ///< 旁支完成计数（跨线程）。
    std::thread::id workerId;  ///< 最后一个执行层的线程。
    std::string strTrace;      ///< 层执行轨迹。

    CSmokeCtx() : nValue(0), nSteps(0), nCatchRuns(0), nFinallyRuns(0), strFailText(), nSide(0)
    {}
};

/// 层：值 +1。
static common::async::CPromiseResult StepAdd1(const std::shared_ptr<CSmokeCtx>& spCtx)
{
    ++spCtx->nValue;
    ++spCtx->nSteps;
    spCtx->workerId = std::this_thread::get_id();
    spCtx->strTrace += "1";
    return common::async::CPromiseResult::Resolve();
}

/// 层：值 +10。
static common::async::CPromiseResult StepAdd10(const std::shared_ptr<CSmokeCtx>& spCtx)
{
    spCtx->nValue += 10;
    ++spCtx->nSteps;
    spCtx->strTrace += "2";
    return common::async::CPromiseResult::Resolve();
}

/// 层：按上下文里的文案制造拒绝。
static common::async::CPromiseResult StepFail(const std::shared_ptr<CSmokeCtx>& spCtx)
{
    ++spCtx->nSteps;
    spCtx->strTrace += "F";
    return common::async::CPromiseResult::Reject(std::runtime_error(spCtx->strFailText));
}

/// 层：记录轨迹（用来验证「失败后不再执行」）。
static common::async::CPromiseResult StepMark(const std::shared_ptr<CSmokeCtx>& spCtx)
{
    ++spCtx->nSteps;
    spCtx->strTrace += "X";
    return common::async::CPromiseResult::Resolve();
}

/// finally 层：只记录轨迹（不看上游结果 —— finally 本来就不需要它）。
static common::async::CPromiseResult StepFinallyMark(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CSmokeCtx>& spCtx)
{
    ++spCtx->nSteps;
    spCtx->strTrace += "X";
    return common::async::CPromiseResult::Resolve();
}

/// 层：抛异常（验证框架捕获 → 异常原样成为本层拒绝，不向调用方抛出）。
static common::async::CPromiseResult StepThrow(const std::shared_ptr<CSmokeCtx>& /*spCtx*/)
{
    throw std::runtime_error("smoke step boom");
}

/// catch 层：透传（补偿后仍让调用方看到失败）。
static common::async::CPromiseResult StepCatchPass(
    common::async::CPromiseResult upResult, const std::shared_ptr<CSmokeCtx>& spCtx)
{
    ++spCtx->nCatchRuns;
    spCtx->strTrace += "c";
    return upResult;
}

/// catch 层：吞掉拒绝并恢复链（后续 then 会继续执行）。
static common::async::CPromiseResult StepCatchRecover(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CSmokeCtx>& spCtx)
{
    ++spCtx->nCatchRuns;
    spCtx->strTrace += "r";
    return common::async::CPromiseResult::Resolve();
}

/// finally 层：成败都执行；返回值被忽略（这里故意返回拒绝，验证不影响结果）。
static common::async::CPromiseResult StepFinallyIgnoreReturn(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CSmokeCtx>& spCtx)
{
    ++spCtx->nFinallyRuns;
    spCtx->strTrace += "f";
    return common::async::CPromiseResult::Reject(std::runtime_error("finally 层的拒绝（应被忽略）"));
}

/// @brief 等旁支完成（旁支是 fire-and-forget，断言前等一下）。
///
/// @param spCtx 上下文。
/// @param nTimeoutMs 最长等待毫秒数。
static void WaitSide(const std::shared_ptr<CSmokeCtx>& spCtx, int nTimeoutMs)
{
    const int nStepMs = 2;
    for (int nWaited = 0; nWaited < nTimeoutMs && spCtx->nSide.load(std::memory_order_relaxed) == 0; nWaited += nStepMs)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(nStepMs));
    }
}

// ==================== 1. 一条链里混用多种写法 ====================

/// @brief 具名处理器 + lambda + 内层链 + 旁支 + catch + finally 串成一条链。
TEST(Smoke_MixedUsagesTrace)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();

    // ② lambda：只此一处用的小逻辑
    common::async::CPromise<CSmokeCtx>::ThenHandler fnLambda = [](const std::shared_ptr<CSmokeCtx>& spSelf)
    {
        ++spSelf->nSteps;
        spSelf->strTrace += "L";
        return common::async::CPromiseResult::Resolve();
    };

    // ④ 工厂：现搭一条内层链，让它参与当前链（同上下文，直接 adopt）
    common::async::CPromise<CSmokeCtx>::PromiseFactory fnInner = [&exec](const std::shared_ptr<CSmokeCtx>& spSelf)
    {
        return exec.NewPromise(spSelf, &StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC).Then(&StepMark, common::async::TaskKind::kWrite, ASYNC_LOC);
    };

    // ⑤ 旁支：普通 Then 里起链但不返回 → 主链不等它
    common::async::CPromise<CSmokeCtx>::ThenHandler fnSide = [&exec](const std::shared_ptr<CSmokeCtx>& spSelf)
    {
        // 旁支用独立上下文，避免与主链并发写同一个 ctx
        std::shared_ptr<CSmokeCtx> spSide = std::make_shared<CSmokeCtx>();
        exec.NewPromise(spSide, &StepMark, common::async::TaskKind::kWrite, ASYNC_LOC)
            .OnSettled(
                [spSelf](common::async::CPromiseResult result)
                {
                    if (result.IsFulfilled())
                    {
                        spSelf->nSide.fetch_add(1, std::memory_order_relaxed);
                    }
                });
        spSelf->strTrace += "s";
        return common::async::CPromiseResult::Resolve();
    };

    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)  // ① 具名函数
                                                .Then(fnLambda, common::async::TaskKind::kWrite, ASYNC_LOC)                // ② lambda
                                                .ThenPromise(fnInner, common::async::TaskKind::kWrite, ASYNC_LOC)          // ④ 内层链（等它）
                                                .Then(fnSide, common::async::TaskKind::kWrite, ASYNC_LOC)                  // ⑤ 旁支（不等）
                                                .Catch(&StepCatchPass, common::async::TaskKind::kWrite, ASYNC_LOC)         // catch（兑现时不执行）
                                                .Finally(&StepFinallyIgnoreReturn, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Await();
    WaitSide(spCtx, 500);

    ASSERT_TRUE(r.IsFulfilled());  // finally 返回拒绝也不改结果
    ASSERT_EQ(spCtx->nValue, 11);  // 1 + 10
    ASSERT_EQ(spCtx->nCatchRuns, 0);
    ASSERT_EQ(spCtx->nFinallyRuns, 1);
    ASSERT_EQ(spCtx->nSide.load(), 1);
    ASSERT_TRUE(spCtx->strTrace == "1L2Xsf");
    exec.Stop();
}

// ==================== 2. ThenPromise：等子 promise ====================

/// @brief 外层一定要等内层链跑完才继续（顺序）。
TEST(Smoke_ThenPromiseWaitsInner)
{
    common::async::CAsyncExecutor exec(1);  // 单 worker 也安全（不占 worker 等待）
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    common::async::CPromise<CSmokeCtx>::PromiseFactory fnInner = [&exec](const std::shared_ptr<CSmokeCtx>& spSelf)
    {
        return exec.NewPromise(spSelf, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC).Then(&StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC);
    };

    const common::async::CPromiseResult r =
        exec.NewPromise(spCtx, &StepMark, common::async::TaskKind::kWrite, ASYNC_LOC).ThenPromise(fnInner, common::async::TaskKind::kWrite, ASYNC_LOC).Then(&StepMark, common::async::TaskKind::kWrite, ASYNC_LOC).Await();

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);
    ASSERT_TRUE(spCtx->strTrace == "X12X");  // 内外层严格交替，外层不会插到内层中间
    exec.Stop();
}

/// @brief 内层链被拒绝：拒绝码作为本层拒绝沿外层链透传，catch / finally 仍执行。
TEST(Smoke_ThenPromiseInnerReject)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    spCtx->strFailText = "业务拒绝（用例 7）";
    common::async::CPromise<CSmokeCtx>::PromiseFactory fnInner = [&exec](const std::shared_ptr<CSmokeCtx>& spSelf)
    {
        return exec.NewPromise(spSelf, &StepFail, common::async::TaskKind::kWrite, ASYNC_LOC);
    };

    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepMark, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .ThenPromise(fnInner, common::async::TaskKind::kWrite, ASYNC_LOC)       // 内层拒绝 → 本层拒绝
                                                .Then(&StepMark, common::async::TaskKind::kWrite, ASYNC_LOC)            // 被跳过
                                                .Catch(&StepCatchPass, common::async::TaskKind::kWrite, ASYNC_LOC)      // 仍执行
                                                .Finally(&StepFinallyMark, common::async::TaskKind::kWrite, ASYNC_LOC)  // 仍执行
                                                .Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Message(), spCtx->strFailText);
    ASSERT_EQ(spCtx->nCatchRuns, 1);
    ASSERT_TRUE(spCtx->strTrace == "XFcX");
    exec.Stop();
}

/// @brief 内层链再套内层链（多层 flatten）。
TEST(Smoke_ThenPromiseDeepInner)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    common::async::CPromise<CSmokeCtx>::PromiseFactory fnDeepest = [&exec](const std::shared_ptr<CSmokeCtx>& spSelf)
    {
        return exec.NewPromise(spSelf, &StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC);
    };
    common::async::CPromise<CSmokeCtx>::PromiseFactory fnMid = [&exec, fnDeepest](const std::shared_ptr<CSmokeCtx>& spSelf)
    {
        return exec.NewPromise(spSelf, &StepMark, common::async::TaskKind::kWrite, ASYNC_LOC).ThenPromise(fnDeepest, common::async::TaskKind::kWrite, ASYNC_LOC);
    };

    const common::async::CPromiseResult r =
        exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC).ThenPromise(fnMid, common::async::TaskKind::kWrite, ASYNC_LOC).Then(&StepMark, common::async::TaskKind::kWrite, ASYNC_LOC).Await();

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);
    ASSERT_TRUE(spCtx->strTrace == "1X2X");
    exec.Stop();
}

// ==================== 3. exec.NewPromise(spCtx, fnStarter)：由外部回调 settle ====================

/// @brief New 的 executor 立即执行，resolve / reject 都由外部决定。
TEST(Smoke_NewExternalSettle)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    // 兑现路径
    {
        std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
        common::async::CPromise<CSmokeCtx>::RejectFn fnRejectHolder;
        common::async::CPromise<CSmokeCtx>::ResolveFn fnResolveHolder;
        common::async::CPromise<CSmokeCtx> p = exec.NewPromise(
            spCtx,

            [&fnResolveHolder, &fnRejectHolder, spCtx](const common::async::CPromise<CSmokeCtx>::ResolveFn& fnResolve,
                const common::async::CPromise<CSmokeCtx>::RejectFn& fnReject)
            {
                fnResolveHolder = fnResolve;  // 存起来，稍后由「外部事件」调用
                fnRejectHolder = fnReject;
                spCtx->strTrace += "n";  // executor 是同步执行的
            },
            common::async::TaskKind::kWrite, ASYNC_LOC);

        ASSERT_TRUE(spCtx->strTrace == "n");  // 立即执行
        ASSERT_TRUE(!p.IsSettled());          // 还没 settle
        fnResolveHolder();                    // 外部 settle
        ASSERT_TRUE(p.Then(&StepMark, common::async::TaskKind::kWrite, ASYNC_LOC).Await().IsFulfilled());
        ASSERT_TRUE(spCtx->strTrace == "nX");
        ASSERT_TRUE(static_cast<bool>(fnRejectHolder));  // reject 句柄有效（本例不用）
    }

    // 拒绝路径
    {
        std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
        common::async::CPromise<CSmokeCtx>::RejectFn fnRejectHolder;
        common::async::CPromise<CSmokeCtx> p = exec.NewPromise(
            spCtx,

            [&fnRejectHolder](const common::async::CPromise<CSmokeCtx>::ResolveFn& /*fnResolve*/,
                const common::async::CPromise<CSmokeCtx>::RejectFn& fnReject)
            {
                fnRejectHolder = fnReject;
            },
            common::async::TaskKind::kWrite, ASYNC_LOC);

        fnRejectHolder(common::async::CPromiseResult::Reject(std::runtime_error("业务拒绝（用例 5）")));
        const common::async::CPromiseResult r = p.Then(&StepMark, common::async::TaskKind::kWrite, ASYNC_LOC).Catch(&StepCatchPass, common::async::TaskKind::kWrite, ASYNC_LOC).Await();
        ASSERT_TRUE(r.IsRejected());
        ASSERT_EQ(r.Message(), std::string("业务拒绝（用例 5）"));
        ASSERT_EQ(spCtx->nCatchRuns, 1);
        ASSERT_EQ(spCtx->nSteps, 0);  // then 层被跳过
    }
    exec.Stop();
}

/// @brief 起链回调抛异常 → 异常原样成为本 promise 的拒绝；空回调 → 同样拒绝。
TEST(Smoke_NewBadExecutor)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtxThrow = std::make_shared<CSmokeCtx>();
    const common::async::CPromiseResult rThrow = exec.NewPromise(
                                                         spCtxThrow,

                                                         [](const common::async::CPromise<CSmokeCtx>::ResolveFn& /*fnResolve*/,
                                                             const common::async::CPromise<CSmokeCtx>::RejectFn& /*fnReject*/)
                                                         {
                                                             throw std::runtime_error("executor boom");
                                                         },
                                                         common::async::TaskKind::kWrite, ASYNC_LOC)
                                                     .Await();
    ASSERT_TRUE(rThrow.IsRejected());
    ASSERT_EQ(rThrow.Message(), std::string("executor boom"));  // 异常原样成为拒绝（what() 都保住）

    std::shared_ptr<CSmokeCtx> spCtxEmpty = std::make_shared<CSmokeCtx>();
    const common::async::CPromiseResult rEmpty =
        exec.NewPromise(spCtxEmpty, common::async::CPromise<CSmokeCtx>::ChainStarter(), common::async::TaskKind::kWrite, ASYNC_LOC).Await();
    ASSERT_TRUE(rEmpty.IsRejected());
    ASSERT_TRUE(asynctest::IsUnspecifiedFailure(rEmpty));  // 未给起链回调：框架侧收口
    exec.Stop();
}

// ==================== 4. 失败即停 / catch / finally ====================

/// @brief then 失败即停：中间层拒绝后，后续 then 不执行，码透传到 Await。
TEST(Smoke_FailFast)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    spCtx->strFailText = "业务拒绝（用例 3）";
    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepMark, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Then(&StepFail, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Then(&StepMark, common::async::TaskKind::kWrite, ASYNC_LOC)  // 跳过
                                                .Then(&StepMark, common::async::TaskKind::kWrite, ASYNC_LOC)  // 跳过
                                                .Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Message(), spCtx->strFailText);
    ASSERT_TRUE(spCtx->strTrace == "XF");
    exec.Stop();
}

/// @brief catch 返回 Resolve() → 吞掉拒绝，链从本层之后继续；最终兑现。
TEST(Smoke_CatchRecoverContinues)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    spCtx->strFailText = "业务拒绝（用例 4）";
    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepFail, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Catch(&StepCatchRecover, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Then(&StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)  // 恢复后继续执行
                                                .Await();

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nCatchRuns, 1);
    ASSERT_EQ(spCtx->nValue, 1);
    ASSERT_TRUE(spCtx->strTrace == "Fr1");
    exec.Stop();
}

/// @brief catch 透传拒绝：补偿完仍让调用方看到失败；后续 then 不执行。
TEST(Smoke_CatchPassthrough)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    spCtx->strFailText = "业务拒绝（用例 5）";
    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepFail, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Catch(&StepCatchPass, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Then(&StepMark, common::async::TaskKind::kWrite, ASYNC_LOC)  // 跳过
                                                .Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Message(), spCtx->strFailText);
    ASSERT_TRUE(spCtx->strTrace == "Fc");
    exec.Stop();
}

/// @brief finally 成败都执行，且返回值被忽略（返回拒绝也不改结果）。
TEST(Smoke_FinallyIgnoresReturn)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    // 兑现路径：finally 返回 Reject 不改结果
    {
        std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
        const common::async::CPromiseResult r =
            exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC).Finally(&StepFinallyIgnoreReturn, common::async::TaskKind::kWrite, ASYNC_LOC).Await();
        ASSERT_TRUE(r.IsFulfilled());
        ASSERT_EQ(spCtx->nFinallyRuns, 1);
    }

    // 拒绝路径：异常原样透传
    {
        std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
        spCtx->strFailText = "业务拒绝（用例 6）";
        const common::async::CPromiseResult r =
            exec.NewPromise(spCtx, &StepFail, common::async::TaskKind::kWrite, ASYNC_LOC).Finally(&StepFinallyIgnoreReturn, common::async::TaskKind::kWrite, ASYNC_LOC).Await();
        ASSERT_TRUE(r.IsRejected());
        ASSERT_EQ(r.Message(), spCtx->strFailText);  // finally 的拒绝被忽略
        ASSERT_EQ(spCtx->nFinallyRuns, 1);
    }
    exec.Stop();
}

/// @brief 处理器抛异常 → 异常原样成为拒绝；catch / finally 仍执行。
TEST(Smoke_HandlerThrowIsException)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepThrow, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Then(&StepMark, common::async::TaskKind::kWrite, ASYNC_LOC)  // 跳过
                                                .Catch(&StepCatchPass, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Finally(&StepFinallyMark, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Message(), std::string("smoke step boom"));  // 处理器抛的异常原样成为拒绝
    ASSERT_EQ(spCtx->nCatchRuns, 1);
    ASSERT_TRUE(spCtx->strTrace == "cX");
    exec.Stop();
}

// ==================== 5. OnSettled：旁路通知 ====================

/// @brief OnSettled 兑现 / 拒绝都通知一次，可多次登记，且不改结果。
TEST(Smoke_OnSettledSideChannel)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    spCtx->strFailText = "业务拒绝（用例 8）";  // 拒绝原因由业务自己定（框架不解释）
    std::atomic<int> nOk(0);
    std::atomic<int> nFail(0);
    common::async::CPromise<CSmokeCtx> p = exec.NewPromise(spCtx, &StepFail, common::async::TaskKind::kWrite, ASYNC_LOC).Catch(&StepCatchPass, common::async::TaskKind::kWrite, ASYNC_LOC);
    p.OnSettled(
        [&nOk, &nFail](common::async::CPromiseResult r)
        {
            if (r.IsRejected())
            {
                nFail.fetch_add(1);
            }
            else
            {
                nOk.fetch_add(1);
            }
        });
    p.OnSettled(
        [&nFail](common::async::CPromiseResult r)
        {
            if (r.IsRejected())
            {
                nFail.fetch_add(1);
            }
        });

    const common::async::CPromiseResult r = p.Await();
    ASSERT_TRUE(r.IsRejected());
    ASSERT_EQ(r.Message(), spCtx->strFailText);  // OnSettled 不吞结果
    // Await 返回只保证「已落定」；已登记的 OnSettled 回调随后在 settle 线程上跑，这里等一下
    for (int nWaited = 0; nWaited < 500 && nFail.load() < 2; nWaited += 2)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(nOk.load(), 0);
    ASSERT_EQ(nFail.load(), 2);  // 两个通知都收到
    exec.Stop();
}

// ==================== 6. Catch / Finally 挂在链首附近 ====================

/// @brief 链首之后紧挂 Catch / Finally：起点已兑现 → Catch 不执行，Finally 照跑。
TEST(Smoke_CatchFinallyRightAfterHead)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Catch(&StepCatchRecover, common::async::TaskKind::kWrite, ASYNC_LOC)  // 兑现 → 不执行
                                                .Finally(&StepFinallyIgnoreReturn, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Await();

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nCatchRuns, 0);
    ASSERT_EQ(spCtx->nFinallyRuns, 1);

    // 独立的 promise：Catch 挂在首层之后（首层起点兑现 → Catch 不执行）。
    std::shared_ptr<CSmokeCtx> spCtx2 = std::make_shared<CSmokeCtx>();
    common::async::CPromise<CSmokeCtx> p2 = exec.NewPromise(spCtx2, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC);
    const common::async::CPromiseResult r2 = p2.Catch(&StepCatchRecover, common::async::TaskKind::kWrite, ASYNC_LOC).Await();
    ASSERT_TRUE(r2.IsFulfilled());
    ASSERT_EQ(spCtx2->nCatchRuns, 0);
    exec.Stop();
}

// ==================== 7. 跨模块桥接（各自执行器） ====================

/// @brief 模块 A 等模块 B 的异步结果：B 在自己执行器上跑，A 桥接等待。
TEST(Smoke_BridgeTwoModules)
{
    common::async::CAsyncExecutor execOrder(2);  // 下单模块（本流程）
    common::async::CAsyncExecutor execStock(1);  // 库存模块（被调方）
    ASSERT_TRUE(execOrder.Start());
    ASSERT_TRUE(execStock.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    std::thread::id idStock;

    // 桥接层：发起库存模块的调用，由它的完成回调 settle 本流程的 promise
    common::async::CPromise<CSmokeCtx>::ChainStarter fnStarter =
        [&execStock, spCtx, &idStock](const common::async::CPromise<CSmokeCtx>::ResolveFn& fnResolve,
            const common::async::CPromise<CSmokeCtx>::RejectFn& fnReject)
    {
        common::async::CPromise<CSmokeCtx> pStock = execStock.NewPromise(spCtx, &StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC);
        pStock.OnSettled(
            [spCtx, fnResolve, fnReject, &idStock](common::async::CPromiseResult result)
            {
                idStock = std::this_thread::get_id();  // 回调跑在库存模块的线程上
                if (result.IsRejected())
                {
                    fnReject(result);  // 整份结果转交（异常类型 + 文案）。
                    return;
                }
                spCtx->strTrace += "b";
                fnResolve();
            });
    };

    // 工厂里才发起跨模块调用（与外层链同步：③ 层被调用时才发起）
    common::async::CPromise<CSmokeCtx>::PromiseFactory fnBridge = [&execOrder, &fnStarter, spCtx](
                                                                      const std::shared_ptr<CSmokeCtx>& /*spSelf*/)
    {
        return execOrder.NewPromise(spCtx, fnStarter, common::async::TaskKind::kWrite, ASYNC_LOC);
    };

    const common::async::CPromiseResult r =
        execOrder.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC).ThenPromise(fnBridge, common::async::TaskKind::kWrite, ASYNC_LOC).Then(&StepMark, common::async::TaskKind::kWrite, ASYNC_LOC).Await();

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);
    ASSERT_TRUE(spCtx->strTrace == "12bX");  // 1 本模块 → 2 库存模块（它自己的线程）→ b 桥接回调 → X 本模块
    ASSERT_TRUE(idStock != std::thread::id());  // 回调确实在（库存模块的）线程上跑过

    execStock.Stop();
    execOrder.Stop();
}

// ==================== 8. 单线程执行器：不阻塞 / 不死锁 ====================

/// @brief 单 worker 下嵌套（内外层同一执行器）+ 旁支也要全部完成。
TEST(Smoke_SingleWorkerNoBlocking)
{
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    common::async::CPromise<CSmokeCtx>::PromiseFactory fnInner = [&exec](const std::shared_ptr<CSmokeCtx>& spSelf)
    {
        return exec.NewPromise(spSelf, &StepAdd10, common::async::TaskKind::kWrite, ASYNC_LOC);
    };

    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .ThenPromise(fnInner, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Then(
                                                    [&exec](const std::shared_ptr<CSmokeCtx>& spSelf)
                                                    {
                                                        // 旁支用独立上下文，避免与主链并发写同一个 ctx
                                                        std::shared_ptr<CSmokeCtx> spSide = std::make_shared<CSmokeCtx>();
                                                        exec.NewPromise(spSide, &StepMark, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                            .OnSettled(
                                                                [spSelf](common::async::CPromiseResult result)
                                                                {
                                                                    if (result.IsFulfilled())
                                                                    {
                                                                        spSelf->nSide.fetch_add(1, std::memory_order_relaxed);
                                                                    }
                                                                });
                                                        spSelf->strTrace += "s";
                                                        return common::async::CPromiseResult::Resolve();
                                                    },
                                                    common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Finally(&StepFinallyIgnoreReturn, common::async::TaskKind::kWrite, ASYNC_LOC)
                                                .Await();
    WaitSide(spCtx, 500);

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 11);
    ASSERT_EQ(spCtx->nSide.load(), 1);
    ASSERT_TRUE(spCtx->strTrace == "12sf");
    exec.Stop();
}

// ==================== 9. 执行器状态与结果类型 ====================

/// @brief 执行器未启动：起链即以框架侧拒绝「执行器已停」收口，Await 不阻塞、不崩。
TEST(Smoke_ChainOnUnstartedExecutor)
{
    common::async::CAsyncExecutor exec(1);  // 故意不 Start
    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    const common::async::CPromiseResult r = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC).Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_TRUE(asynctest::IsStoppedFailure(r));  // 执行器未启动 → 框架侧拒绝「执行器已停」
    ASSERT_EQ(spCtx->nSteps, 0);
}

/// @brief IsSettled / GetContext / CPromiseResult 的基本语义。
TEST(Smoke_PromiseIntrospection)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CSmokeCtx> spCtx = std::make_shared<CSmokeCtx>();
    common::async::CPromise<CSmokeCtx> p = exec.NewPromise(spCtx, &StepAdd1, common::async::TaskKind::kWrite, ASYNC_LOC);
    ASSERT_TRUE(p.GetContext() == spCtx);  // 全链共用同一实例
    const common::async::CPromiseResult r = p.Await();
    ASSERT_TRUE(p.IsSettled());

    ASSERT_TRUE(r.IsFulfilled());
    ASSERT_TRUE(!r.IsRejected());
    ASSERT_TRUE(r.Message().empty());  // 兑现：没有异常
    ASSERT_TRUE(r == common::async::CPromiseResult::Resolve());
    ASSERT_TRUE(common::async::CPromiseResult::Reject(std::runtime_error("A")) !=
                common::async::CPromiseResult::Reject(std::runtime_error("B")));
    ASSERT_TRUE(p.GetContext() == spCtx);
    exec.Stop();
}
