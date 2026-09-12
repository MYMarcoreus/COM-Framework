/// @file test_async_affinity_override.cpp
/// 改进 B（逐层覆盖默认线程亲和）的专项测试。
///
/// 默认：**每一层都在本链执行器线程上**（改进 A 的线程亲和）。
/// 覆盖手段（本文件验收）：
///  - `ThenInline(handler)`：本层在**结算线程**上就地执行（不投递、不要求亲和）——
///    跨模块返回后想直接跑在被调模块线程上时用（只做与对方相关的轻活）；
///  - `ThenOn(exec, handler)`：本层在**指定执行器**线程上执行（已在该线程则就地，否则投递）——
///    例如放到本模块的另一个执行器上跑；
///  - 两者之后的层（默认亲和）**会切回本链执行器**；
///  - 指定执行器不可用（已 Stop）→ 本层以 `kStopped` 收口，不挂死、后续层跳过。

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

// ==================== 手动放行的门 ====================

// ==================== 被调模块（1 线程） ====================

/// @brief 被调模块上下文。
struct COverrideCalleeCtx
{
    int nAvail;               ///< 出参：可用库存。
    std::atomic<int> nSteps;  ///< 已执行步骤数。
    std::thread::id idStep;   ///< 步骤所在线程。

    COverrideCalleeCtx() : nAvail(5), nSteps(0)
    {}
};

/// @brief 被调模块：自持 1 线程执行器。
class COverrideCalleeModule
{
public:
    COverrideCalleeModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 查询（一层）。
    ///
    /// @param spCtx 本模块上下文。
    ///
    /// @return 本层 promise。
    common::async::CPromise<COverrideCalleeCtx> QueryAsync(const std::shared_ptr<COverrideCalleeCtx>& spCtx)
    {
        return m_exec.NewPromise(spCtx, &StepQuery, ASYNC_LOC);
    }

private:
    /// 层处理器：记录线程与次数。
    static common::async::CPromiseResult StepQuery(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<COverrideCalleeCtx>& spCtx)
    {
        ++spCtx->nSteps;
        spCtx->idStep = std::this_thread::get_id();
        return common::async::CPromiseResult::Resolve();
    }

    common::async::CAsyncExecutor m_exec;  ///< 模块私有执行器（单线程）。
};

// ==================== 调用方模块（主 + 旁路两个执行器） ====================

/// @brief 调用方上下文。
struct COverrideOrderCtx
{
    int nOwnSteps;                         ///< 本模块自有层执行次数。
    std::thread::id idSettler;             ///< 结算「子 promise」所在线程（由用例选定）。
    int nCatchRuns;                        ///< catch 执行次数。
    int nCaughtCode;                       ///< catch 收到的码。
    std::string strTrace;                  ///< 层轨迹。
    std::thread::id idFirst;               ///< 首层所在线程（主执行器线程）。
    std::thread::id idAfterBridgeInline;   ///< `ThenInline` 层所在线程（应在结算线程）。
    std::thread::id idAfterBridgeDefault;  ///< 默认亲和层所在线程（应在主执行器线程）。
    std::thread::id idOnSide;              ///< `ThenOn(旁路执行器)` 层所在线程。
    std::thread::id idAfterSide;           ///< `ThenOn` 之后那一层所在线程（应回主执行器）。
    std::thread::id idLastOwn;             ///< 最后一个自有层所在线程。

    COverrideOrderCtx() : nOwnSteps(0), nCatchRuns(0), nCaughtCode(0)
    {}
};

/// @brief 调用方模块：主执行器 + 旁路执行器。
class COverrideOrderModule
{
public:
    COverrideOrderModule() : m_execMain(1), m_execSide(1)
    {
        m_execMain.Start();
        m_execSide.Start();
    }

    /// @brief 停止旁路执行器（供「指定执行器不可用」的用例）。
    void StopSide()
    {
        m_execSide.Stop();
    }

    /// @brief 用 `ThenInline` 覆盖：上游在**旁路执行器线程**上结算，本层应就地跑在那条线程上。
    ///
    /// 为什么上游自己建：`ThenInline` 的语义是「在**结算上游的那条线程**上就地执行」，而跨模块桥接
    /// 的结算线程本身是**二选一**的（子 promise 若在 `Adopt` 注册通知前就已落定，通知会被投递回
    /// 本链执行器 → 那一层就跑在本链线程上）—— 用例要断言线程，就只能由自己指定结算线程，
    /// 否则就是赌时序（本用例早期版本靠「固定延时」赌，在 TSan 慢环境下会偶发失败）。
    /// 跨模块的真实形状由 `RunDefaultAsync` / `RunOnSideAsync` 与 `test_async_affinity.cpp` 覆盖。
    common::async::CPromise<COverrideOrderCtx> RunInlineWithSideChildAsync(
        const std::shared_ptr<COverrideOrderCtx>& spCtx)
    {
        // 子 promise：挂在**旁路执行器**上，由本方法最后投递的那次调用结算。
        typedef common::async::CPromise<COverrideOrderCtx> COrderPromise;
        COrderPromise::ResolveFn fnSettleChild;
        const COrderPromise promiseChild = COrderPromise::New(
            m_execSide, spCtx,
            [&fnSettleChild](const COrderPromise::ResolveFn& fnResolve, const COrderPromise::RejectFn&)
            {
                fnSettleChild = fnResolve;  // 先存起来，稍后在旁路线程上结算。
            },
            ASYNC_LOC);

        common::async::CPromise<COverrideOrderCtx> chain =
            m_execMain.NewPromise(spCtx, &StepOrderLoad, ASYNC_LOC)
                .ThenPromise(
                    [promiseChild](const std::shared_ptr<COverrideOrderCtx>&)
                    {
                        return promiseChild;
                    },
                    ASYNC_LOC)
                .ThenInline(&StepAfterBridgeInline, ASYNC_LOC)  // ← 覆盖：就地（结算线程）
                .Then(&StepAfterBridgeDefault, ASYNC_LOC)       // ← 默认亲和：回主执行器
                .Catch(&StepCatch, ASYNC_LOC);

        // 层已挂完（`Adopt` 已登记通知）→ 再在旁路执行器线程上结算子 promise，结算线程确定。
        m_execSide.Post(
            [fnSettleChild, spCtx]()
            {
                spCtx->idSettler = std::this_thread::get_id();
                fnSettleChild();
            });
        return chain;
    }

    /// @brief 对照：同样位置用默认 `Then`（应回主执行器线程）。
    common::async::CPromise<COverrideOrderCtx> RunDefaultAsync(
        const std::shared_ptr<COverrideOrderCtx>& spCtx, const std::shared_ptr<COverrideCalleeModule>& spCallee)
    {
        return m_execMain.NewPromise(spCtx, &StepOrderLoad, ASYNC_LOC)
            .ThenPromise(MakeCallFactory(spCallee), ASYNC_LOC)
            .Then(&StepAfterBridgeDefault, ASYNC_LOC)  // ← 默认亲和
            .Catch(&StepCatch, ASYNC_LOC);
    }

    /// @brief 用 `ThenOn` 指定旁路执行器：该层跑在旁路执行器线程上，之后的层切回主执行器。
    common::async::CPromise<COverrideOrderCtx> RunOnSideAsync(
        const std::shared_ptr<COverrideOrderCtx>& spCtx, const std::shared_ptr<COverrideCalleeModule>& spCallee)
    {
        return m_execMain.NewPromise(spCtx, &StepOrderLoad, ASYNC_LOC)
            .ThenPromise(MakeCallFactory(spCallee), ASYNC_LOC)
            .ThenOn(m_execSide, &StepOnSide, ASYNC_LOC)  // ← 覆盖：指定执行器
            .Then(&StepAfterSide, ASYNC_LOC)             // ← 默认亲和：回主执行器
            .Catch(&StepCatch, ASYNC_LOC);
    }

    /// @brief 用 `ThenOn` 指定一个**已停止**的执行器：本层应以 kStopped 收口。
    common::async::CPromise<COverrideOrderCtx> RunOnStoppedAsync(const std::shared_ptr<COverrideOrderCtx>& spCtx)
    {
        return m_execMain.NewPromise(spCtx, &StepOrderLoad, ASYNC_LOC)
            .ThenOn(m_execSide, &StepOnSide, ASYNC_LOC)  // 旁路执行器已 Stop
            .Then(&StepAfterSide, ASYNC_LOC)
            .Catch(&StepCatch, ASYNC_LOC);
    }

private:
    /// 跨模块桥接层的工厂（本层属于本模块；被调模块在自己执行器上跑）。
    common::async::CPromise<COverrideOrderCtx>::PromiseFactory MakeCallFactory(
        const std::shared_ptr<COverrideCalleeModule>& spCallee)
    {
        return [this, spCallee](const std::shared_ptr<COverrideOrderCtx>& spSelf)
        {
            return BridgeCallCallee(spSelf, spCallee);
        };
    }

    /// ① 本模块自有层（主执行器）。
    static common::async::CPromiseResult StepOrderLoad(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<COverrideOrderCtx>& spCtx)
    {
        ++spCtx->nOwnSteps;
        spCtx->idFirst = std::this_thread::get_id();
        spCtx->idLastOwn = spCtx->idFirst;
        spCtx->strTrace += "A1;";
        return common::async::CPromiseResult::Resolve();
    }

    /// ③ `ThenInline` 层：应在被调模块线程上（就地）。
    static common::async::CPromiseResult StepAfterBridgeInline(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<COverrideOrderCtx>& spCtx)
    {
        spCtx->idAfterBridgeInline = std::this_thread::get_id();
        spCtx->strTrace += "A2i;";
        return common::async::CPromiseResult::Resolve();
    }

    /// ④ 默认亲和层：应回主执行器线程。
    static common::async::CPromiseResult StepAfterBridgeDefault(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<COverrideOrderCtx>& spCtx)
    {
        ++spCtx->nOwnSteps;
        spCtx->idAfterBridgeDefault = std::this_thread::get_id();
        spCtx->idLastOwn = spCtx->idAfterBridgeDefault;
        spCtx->strTrace += "A3;";
        return common::async::CPromiseResult::Resolve();
    }

    /// `ThenOn(旁路执行器)` 层：应在旁路执行器线程上。
    static common::async::CPromiseResult StepOnSide(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<COverrideOrderCtx>& spCtx)
    {
        spCtx->idOnSide = std::this_thread::get_id();
        spCtx->strTrace += "S;";
        return common::async::CPromiseResult::Resolve();
    }

    /// `ThenOn` 之后那一层：应切回主执行器线程。
    static common::async::CPromiseResult StepAfterSide(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<COverrideOrderCtx>& spCtx)
    {
        ++spCtx->nOwnSteps;
        spCtx->idAfterSide = std::this_thread::get_id();
        spCtx->idLastOwn = spCtx->idAfterSide;
        spCtx->strTrace += "A3;";
        return common::async::CPromiseResult::Resolve();
    }

    /// 兜底层。
    static common::async::CPromiseResult StepCatch(
        common::async::CPromiseResult upResult, const std::shared_ptr<COverrideOrderCtx>& spCtx)
    {
        ++spCtx->nCatchRuns;
        spCtx->nCaughtCode = upResult.Code();
        spCtx->strTrace += "C;";
        return upResult;
    }

    /// 桥接层：把被调模块的 promise 接进本流程（不检查 OnSettled 返回值也安全）。
    common::async::CPromise<COverrideOrderCtx> BridgeCallCallee(
        const std::shared_ptr<COverrideOrderCtx>& spCtx, const std::shared_ptr<COverrideCalleeModule>& spCallee)
    {
        common::async::CPromise<COverrideOrderCtx>::PromiseExecutor fnExecutor =
            [spCallee, spCtx](const common::async::CPromise<COverrideOrderCtx>::ResolveFn& fnResolve,
                const common::async::CPromise<COverrideOrderCtx>::RejectFn& fnReject)
        {
            auto spCalleeCtx = std::make_shared<COverrideCalleeCtx>();
            common::async::CPromise<COverrideCalleeCtx> promiseCallee = spCallee->QueryAsync(spCalleeCtx);
            promiseCallee.OnSettled(
                [spCtx, spCalleeCtx, fnResolve, fnReject](common::async::CPromiseResult result)
                {
                    if (result.IsRejected())
                    {
                        fnReject(result.Code());
                        return;
                    }
                    spCtx->strTrace += "B1;";
                    fnResolve();
                });
        };
        return common::async::CPromise<COverrideOrderCtx>::New(m_execMain, spCtx, fnExecutor, ASYNC_LOC);
    }

    common::async::CAsyncExecutor m_execMain;  ///< 主执行器（链的主执行器）。
    common::async::CAsyncExecutor m_execSide;  ///< 旁路执行器（`ThenOn` 用）。
};

// ==================== 用例 ====================

/// @brief `ThenInline`：本层就地跑在「结算上游的那条线程」上；之后的层（默认亲和）切回本链执行器。
TEST(AffinityOverride_ThenInlineRunsOnSettleThread)
{
    auto spOrder = std::make_shared<COverrideOrderModule>();
    auto spCtx = std::make_shared<COverrideOrderCtx>();

    // 结算线程 = 旁路执行器线程（由 RunInlineWithSideChildAsync 内部在挂完层后投递的那次调用决定）
    ASSERT_TRUE(spOrder->RunInlineWithSideChildAsync(spCtx).Await().IsFulfilled());

    ASSERT_EQ(spCtx->strTrace, std::string("A1;A2i;A3;"));        // 顺序不乱
    ASSERT_TRUE(spCtx->idAfterBridgeInline == spCtx->idSettler);  // 覆盖成功：就地（结算线程）
    ASSERT_TRUE(spCtx->idAfterBridgeInline != spCtx->idFirst);    // 不在本链执行器上
    ASSERT_TRUE(spCtx->idAfterBridgeDefault == spCtx->idFirst);   // 下一层回本链执行器
    ASSERT_EQ(spCtx->nOwnSteps, 2);
    ASSERT_EQ(spCtx->nCatchRuns, 0);
}

/// @brief 对照：同一位置用默认 `Then` → 恒在主执行器线程（证明覆盖是显式的）。
TEST(AffinityOverride_DefaultThenStaysOnOwnExecutor)
{
    auto spCallee = std::make_shared<COverrideCalleeModule>();
    auto spOrder = std::make_shared<COverrideOrderModule>();
    auto spCtx = std::make_shared<COverrideOrderCtx>();

    ASSERT_TRUE(spOrder->RunDefaultAsync(spCtx, spCallee).Await().IsFulfilled());

    ASSERT_EQ(spCtx->strTrace, std::string("A1;B1;A3;"));
    ASSERT_TRUE(spCtx->idAfterBridgeDefault == spCtx->idFirst);
    ASSERT_EQ(spCtx->nOwnSteps, 2);
}

/// @brief `ThenOn`：本层跑在指定执行器线程上；之后的层切回主执行器。
TEST(AffinityOverride_ThenOnRunsOnGivenExecutor)
{
    auto spCallee = std::make_shared<COverrideCalleeModule>();
    auto spOrder = std::make_shared<COverrideOrderModule>();
    auto spCtx = std::make_shared<COverrideOrderCtx>();

    ASSERT_TRUE(spOrder->RunOnSideAsync(spCtx, spCallee).Await().IsFulfilled());

    ASSERT_EQ(spCtx->strTrace, std::string("A1;B1;S;A3;"));
    ASSERT_TRUE(spCtx->idOnSide != spCtx->idFirst);     // 不在主执行器上（在旁路执行器上）
    ASSERT_TRUE(spCtx->idAfterSide == spCtx->idFirst);  // 下一层回主执行器
    ASSERT_EQ(spCtx->nOwnSteps, 2);
}

/// @brief `ThenOn` 指定已停止的执行器：本层以 kStopped 收口，后续层跳过、catch 兜底（不挂死）。
TEST(AffinityOverride_ThenOnStoppedExecutorRejects)
{
    auto spOrder = std::make_shared<COverrideOrderModule>();
    auto spCtx = std::make_shared<COverrideOrderCtx>();

    spOrder->StopSide();  // 旁路执行器不可用

    const common::async::CPromiseResult result = spOrder->RunOnStoppedAsync(spCtx).Await();

    ASSERT_TRUE(result.IsRejected());
    ASSERT_EQ(result.Code(), common::async::kStopped);
    ASSERT_EQ(spCtx->strTrace, std::string("A1;C;"));
    ASSERT_EQ(spCtx->nOwnSteps, 1);  // 只有 StepOrderLoad 执行（S / A3 都没跑）
    ASSERT_EQ(spCtx->nCatchRuns, 1);
    ASSERT_EQ(spCtx->nCaughtCode, common::async::kStopped);
}
