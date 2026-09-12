/// @file test_async_build_start.cpp
/// 改进 C（build-then-start）的专项测试：`BuildPromise` + `Start()`。
///
/// 承诺（本文件即验收标准）：
///  - 构链期间**不跑任何业务代码**（包括跨模块调用都不发起）；
///  - `Start()` 之后首层才投递，链按依赖边顺序推进，线程归属照旧（每层在自己的模块线程上）；
///  - `Start()` 幂等；漏写 `Start()` 直接 `Await()` 会自动启动（兜底，不死等）；
///  - `Start()` 之后再追加层仍可用（走亲和：回本链执行器线程）；
///  - 空链（没挂过任何层）不会崩：`Await()` 仍是既有的「无效句柄」语义（kStopped）。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "TestFramework.h"

// 跨模块桥接（ThenBridge）用例会用到延迟链：证明「Start 之前连子链都不发起」。

// ==================== 被调模块（1 线程） ====================

/// @brief 被调模块上下文（用于证明构链期确实没被调用）。
struct CStartCalleeCtx
{
    int nAvail;               ///< 出参：可用库存。
    std::atomic<int> nSteps;  ///< 已执行步骤数。
    std::thread::id idStep;   ///< 步骤所在线程。

    CStartCalleeCtx() : nAvail(5), nSteps(0)
    {}
};

/// @brief 被调模块：自持 1 线程执行器。
class CStartCalleeModule
{
public:
    CStartCalleeModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 查询（一层）。
    ///
    /// @param spCtx 本模块上下文。
    ///
    /// @return 本层 promise。
    common::async::CPromise<CStartCalleeCtx> QueryAsync(const std::shared_ptr<CStartCalleeCtx>& spCtx)
    {
        return m_exec.NewPromise(spCtx, &StepQuery, ASYNC_LOC);
    }

private:
    /// 层处理器。
    static common::async::CPromiseResult StepQuery(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CStartCalleeCtx>& spCtx)
    {
        ++spCtx->nSteps;
        spCtx->idStep = std::this_thread::get_id();
        return common::async::CPromiseResult::Resolve();
    }

    common::async::CAsyncExecutor m_exec;  ///< 模块私有执行器（单线程）。
};

// ==================== 调用方模块（1 线程） ====================

/// @brief 调用方上下文。
struct CStartCtx
{
    int nOwnSteps;                  ///< 本模块自有层执行次数。
    int nCountSteps;                ///< 深链计数层执行次数。
    int nStock;                     ///< 从被调模块带回来的值。
    std::string strTrace;           ///< 层轨迹。
    std::thread::id idFirst;        ///< 首层所在线程。
    std::thread::id idAfterBridge;  ///< 跨模块返回后那一层所在线程。
    std::thread::id idLastOwn;      ///< 最后一个自有层所在线程。

    CStartCtx() : nOwnSteps(0), nCountSteps(0), nStock(0)
    {}
};

/// @brief 调用方模块：链用 `BuildPromise` 建（延迟启动）。
class CStartOrderModule
{
public:
    CStartOrderModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 建一条跨模块链（**不启动**）：A1 → 等被调模块 → A2 → A3。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spCallee 被调模块。
    ///
    /// @return 未启动的链句柄。
    common::async::CPromise<CStartCtx> BuildChain(
        const std::shared_ptr<CStartCtx>& spCtx, const std::shared_ptr<CStartCalleeModule>& spCallee)
    {
        common::async::CPromise<CStartCtx>::PromiseFactory fnCall = [this, spCallee](
                                                                        const std::shared_ptr<CStartCtx>& spSelf)
        {
            return BridgeCall(spSelf, spCallee);
        };

        return m_exec.BuildPromise(spCtx)
            .Then(&StepLoad, ASYNC_LOC)
            .ThenPromise(fnCall, ASYNC_LOC)
            .Then(&StepAfterBridge, ASYNC_LOC)
            .Then(&StepFinal, ASYNC_LOC);
    }

    /// @brief 建一条深链（**不启动**）：nLayers 层纯计数层。
    ///
    /// @param spCtx 本流程上下文。
    /// @param nLayers 层数。
    ///
    /// @return 未启动的链句柄。
    common::async::CPromise<CStartCtx> BuildDeepChain(const std::shared_ptr<CStartCtx>& spCtx, int nLayers)
    {
        common::async::CPromise<CStartCtx> promise = m_exec.BuildPromise(spCtx).Then(&StepCount, ASYNC_LOC);
        for (int i = 1; i < nLayers; ++i)
        {
            promise = promise.Then(&StepCount, ASYNC_LOC);
        }
        return promise;
    }

    /// @brief 在已有链尾追加一层（可用于 `Start()` 之后追加）。
    ///
    /// @param promise 当前链尾句柄（按值传入，因为要在它上面追加层）。
    ///
    /// @return 追加后的新层句柄。
    common::async::CPromise<CStartCtx> AppendFinal(common::async::CPromise<CStartCtx> promise)
    {
        return promise.Then(&StepFinal, ASYNC_LOC);
    }

private:
    /// ① 本模块自有层。
    static common::async::CPromiseResult StepLoad(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CStartCtx>& spCtx)
    {
        ++spCtx->nOwnSteps;
        spCtx->idFirst = std::this_thread::get_id();
        spCtx->idLastOwn = spCtx->idFirst;
        spCtx->strTrace += "A1;";
        return common::async::CPromiseResult::Resolve();
    }

    /// ③ 跨模块返回后的层（默认亲和 → 回本模块执行器）。
    static common::async::CPromiseResult StepAfterBridge(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CStartCtx>& spCtx)
    {
        spCtx->idAfterBridge = std::this_thread::get_id();
        spCtx->strTrace += "A2;";
        return common::async::CPromiseResult::Resolve();
    }

    /// ④ 收尾层。
    static common::async::CPromiseResult StepFinal(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CStartCtx>& spCtx)
    {
        ++spCtx->nOwnSteps;
        spCtx->idLastOwn = std::this_thread::get_id();
        spCtx->strTrace += "A3;";
        return common::async::CPromiseResult::Resolve();
    }

    /// 深链计数层。
    static common::async::CPromiseResult StepCount(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CStartCtx>& spCtx)
    {
        ++spCtx->nCountSteps;
        spCtx->idLastOwn = std::this_thread::get_id();
        return common::async::CPromiseResult::Resolve();
    }

    /// 跨模块桥接层（本层属于本模块；被调模块在自己执行器上跑）。
    common::async::CPromise<CStartCtx> BridgeCall(
        const std::shared_ptr<CStartCtx>& spCtx, const std::shared_ptr<CStartCalleeModule>& spCallee)
    {
        common::async::CPromise<CStartCtx>::PromiseExecutor fnExecutor =
            [spCallee, spCtx](const common::async::CPromise<CStartCtx>::ResolveFn& fnResolve,
                const common::async::CPromise<CStartCtx>::RejectFn& fnReject)
        {
            auto spCalleeCtx = std::make_shared<CStartCalleeCtx>();
            common::async::CPromise<CStartCalleeCtx> promiseCallee = spCallee->QueryAsync(spCalleeCtx);
            promiseCallee.OnSettled(
                [spCtx, spCalleeCtx, fnResolve, fnReject](common::async::CPromiseResult result)
                {
                    if (result.IsRejected())
                    {
                        fnReject(result.Code());
                        return;
                    }
                    spCtx->nStock = spCalleeCtx->nAvail;
                    spCtx->strTrace += "B1;";
                    fnResolve();
                });
        };
        return m_exec.NewPromise(spCtx, fnExecutor, ASYNC_LOC);
    }

    common::async::CAsyncExecutor m_exec;  ///< 模块私有执行器（单线程）。
};

// ==================== 用例 ====================

/// @brief 构链期间什么都不跑（连跨模块调用都不发起）；`Start()` 之后才开跑。
TEST(BuildStart_NothingRunsBeforeStart)
{
    auto spCallee = std::make_shared<CStartCalleeModule>();
    auto spOrder = std::make_shared<CStartOrderModule>();
    auto spCtx = std::make_shared<CStartCtx>();

    common::async::CPromise<CStartCtx> promise = spOrder->BuildChain(spCtx, spCallee);

    ASSERT_TRUE(promise.IsDeferred());
    ASSERT_TRUE(promise.IsStarted() == false);

    // 给足时间：如果构链期会跑，这段时间足够跑完
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_EQ(spCtx->nOwnSteps, 0);
    ASSERT_EQ(spCtx->strTrace, std::string(""));
    ASSERT_EQ(spCtx->nStock, 0);

    // 启动后按依赖边顺序跑完
    promise.Start();
    ASSERT_TRUE(promise.IsStarted());
    ASSERT_TRUE(promise.Await().IsFulfilled());

    ASSERT_EQ(spCtx->strTrace, std::string("A1;B1;A2;A3;"));
    ASSERT_EQ(spCtx->nOwnSteps, 2);  // A1 + A3（StepAfterBridge 不计数）
    ASSERT_EQ(spCtx->nStock, 5);
    ASSERT_TRUE(spCtx->idAfterBridge == spCtx->idFirst);  // 跨模块返回层仍回本模块执行器
}

/// @brief `Start()` 幂等：连调两次每层也只跑一次。
TEST(BuildStart_StartIsIdempotent)
{
    auto spCallee = std::make_shared<CStartCalleeModule>();
    auto spOrder = std::make_shared<CStartOrderModule>();
    auto spCtx = std::make_shared<CStartCtx>();

    common::async::CPromise<CStartCtx> promise = spOrder->BuildChain(spCtx, spCallee);
    promise.Start();
    promise.Start();
    promise.Start();

    ASSERT_TRUE(promise.Await().IsFulfilled());
    ASSERT_EQ(spCtx->strTrace, std::string("A1;B1;A2;A3;"));
    ASSERT_EQ(spCtx->nOwnSteps, 2);
}

/// @brief 漏写 `Start()` 直接 `Await()`：自动启动（兜底，不死等）。
TEST(BuildStart_AwaitAutoStarts)
{
    auto spCallee = std::make_shared<CStartCalleeModule>();
    auto spOrder = std::make_shared<CStartOrderModule>();
    auto spCtx = std::make_shared<CStartCtx>();

    common::async::CPromise<CStartCtx> promise = spOrder->BuildChain(spCtx, spCallee);
    ASSERT_TRUE(promise.IsStarted() == false);

    ASSERT_TRUE(promise.Await().IsFulfilled());  // 未显式 Start
    ASSERT_TRUE(promise.IsStarted());

    ASSERT_EQ(spCtx->strTrace, std::string("A1;B1;A2;A3;"));
    ASSERT_EQ(spCtx->nOwnSteps, 2);
}

/// @brief `Start()` 之后追加层仍可用：新层回本链执行器线程执行。
TEST(BuildStart_LateAppendAfterStart)
{
    auto spCallee = std::make_shared<CStartCalleeModule>();
    auto spOrder = std::make_shared<CStartOrderModule>();
    auto spCtx = std::make_shared<CStartCtx>();

    common::async::CPromise<CStartCtx> promise = spOrder->BuildChain(spCtx, spCallee);
    promise.Start();

    // 启动后追加一层（上一层可能已 settled，也可能还在跑）
    common::async::CPromise<CStartCtx> promiseTail = spOrder->AppendFinal(promise);

    ASSERT_TRUE(promiseTail.Await().IsFulfilled());
    ASSERT_EQ(spCtx->strTrace, std::string("A1;B1;A2;A3;A3;"));
    ASSERT_EQ(spCtx->nOwnSteps, 3);                   // A1 + 两次 A3
    ASSERT_TRUE(spCtx->idLastOwn == spCtx->idFirst);  // 追加层也在本模块执行器上
}

/// @brief 深链（5000 层）延迟构建后启动：全部执行、不爆栈、全在同一执行器线程。
TEST(BuildStart_DeepChain)
{
    const int nLayers = 5000;

    auto spOrder = std::make_shared<CStartOrderModule>();
    auto spCtx = std::make_shared<CStartCtx>();

    common::async::CPromise<CStartCtx> promise = spOrder->BuildDeepChain(spCtx, nLayers);

    ASSERT_EQ(spCtx->nCountSteps, 0);  // 构链期没跑
    promise.Start();
    ASSERT_TRUE(promise.Await().IsFulfilled());

    ASSERT_EQ(spCtx->nCountSteps, nLayers);
    ASSERT_TRUE(spCtx->idLastOwn != std::thread::id());  // 确实在某个工作线程上跑过
}

/// @brief 空链（没挂过任何层）：`Start()` / `Await()` 不崩，语义与既有的无效句柄一致。
TEST(BuildStart_EmptyChain)
{
    auto spOrder = std::make_shared<CStartOrderModule>();
    auto spCtx = std::make_shared<CStartCtx>();

    common::async::CAsyncExecutor execUnused(1);
    common::async::CPromise<CStartCtx> promiseEmpty = execUnused.BuildPromise(spCtx);  // 没挂任何层
    promiseEmpty.Start();
    promiseEmpty.Start();  // 幂等

    const common::async::CPromiseResult result = promiseEmpty.Await();
    ASSERT_TRUE(result.IsRejected());
    ASSERT_EQ(result.Code(), common::async::kStopped);  // 空链 = 没有层（句柄无状态）
}

/// @brief 延迟链 + `ThenBridge`：`Start()` 之前**连跨模块子链都不发起**（构链期零业务代码）。
///
/// 回归点：`exec.NewPromise(spCtx, executor)` 系列（含 `ThenBridge` 内部）恒为「立即启动」——
/// 它的等待语义由「轮到该层」保证，不能被延迟链的 `bDeferred` 影响。
TEST(BuildStart_BridgeWaitsForStart)
{
    auto spCallee = std::make_shared<CStartCalleeModule>();
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    auto spCtx = std::make_shared<CStartCtx>();
    auto spCalleeCtx = std::make_shared<CStartCalleeCtx>();

    common::async::CPromise<CStartCtx>::ThenHandler fnFirst =
        [](common::async::CPromiseResult, const std::shared_ptr<CStartCtx>& spCtx)
    {
        ++spCtx->nOwnSteps;
        spCtx->strTrace += "A1;";
        spCtx->idFirst = std::this_thread::get_id();
        return common::async::CPromiseResult::Resolve();
    };
    auto fnCreateCallee = [spCallee, spCalleeCtx](
                              const std::shared_ptr<CStartCtx>&) -> common::async::CPromise<CStartCalleeCtx>
    {
        return spCallee->QueryAsync(spCalleeCtx);  // 只有 Start 之后才会被调用
    };
    auto fnApplyCallee = [](const std::shared_ptr<CStartCtx>& spSelf, const std::shared_ptr<CStartCalleeCtx>& spOther)
    {
        spSelf->nStock = spOther->nAvail;
    };
    common::async::CPromise<CStartCtx>::ThenHandler fnAppend =
        [](common::async::CPromiseResult, const std::shared_ptr<CStartCtx>& spCtx)
    {
        ++spCtx->nOwnSteps;
        spCtx->strTrace += "A2;";
        spCtx->idAfterBridge = std::this_thread::get_id();
        return common::async::CPromiseResult::Resolve();
    };

    common::async::CPromise<CStartCtx> promise = exec.BuildPromise(spCtx)
                                                     .Then(fnFirst, ASYNC_LOC)
                                                     .ThenBridge(fnCreateCallee, fnApplyCallee, ASYNC_LOC)
                                                     .Then(fnAppend, ASYNC_LOC);

    ASSERT_TRUE(promise.IsDeferred());
    ASSERT_TRUE(!promise.IsStarted());
    ASSERT_EQ(spCtx->nOwnSteps, 0);            // 构链期：本模块层没跑
    ASSERT_EQ(spCalleeCtx->nSteps.load(), 0);  // 构链期：跨模块子链也没发起
    ASSERT_EQ(spCtx->strTrace, std::string(""));

    promise.Start();
    ASSERT_TRUE(promise.Await().IsFulfilled());

    ASSERT_EQ(spCtx->strTrace, std::string("A1;A2;"));
    ASSERT_EQ(spCtx->nOwnSteps, 2);
    ASSERT_EQ(spCtx->nStock, 5);  // 跨模块数据已搬回
    ASSERT_EQ(spCalleeCtx->nSteps.load(), 1);
    ASSERT_TRUE(spCtx->idAfterBridge == spCtx->idFirst);  // 线程亲和不变
    exec.Stop();
}
