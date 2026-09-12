/// @file test_async_modules_stress.cpp
/// 跨模块异步调用的极限测试：每模块单线程执行器（1 worker）下的高并发、深链、汇聚与退化。
///
/// 与基础用例（Tests/test_async_modules.cpp）互补，这里压的是「极限」：
///  - 200 条链同时跨模块调用一个单线程模块（不丢步、不串线程、模块内不重叠）；
///  - 单条链 100 轮 A↔B 往返（跨模块 + 回本模块交替），顺序与线程归属逐轮校验；
///  - 单条链 5000 层（单线程执行器全内联级联，压 kMaxInlineDepth 防爆栈）；
///  - 8 个线程同时 Await 同一条链（notify_all，链只跑一遍）；
///  - 一层分叉 64 条跨模块分支 + 手写汇聚（等价 when_all），汇聚层也回本模块线程；
///  - 100 条链一半让被调模块拒绝（成功/失败互不串）；
///  - 链跑到一半把被调模块执行器 Stop（退化为 kStopped 拒绝，不挂死、后续步骤全跳过）。
///
/// 共享观测只用原子量或「每条链自己的」数据结构，避免测试自身引入数据竞争（可过 TSan）。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "AsyncTestKit.h"
#include "TestFramework.h"

// ==================== 规模参数 ====================

static const int kStressChains = 200;       ///< 并发链数。
static const int kStressRounds = 100;       ///< 单链往返轮数。
static const int kStressDeepLayers = 5000;  ///< 单链层数（单模块）。
static const int kStressBranches = 64;      ///< 一层分叉的分支数。
static const int kStressWaiters = 8;        ///< 同时 Await 同一链的线程数。
static const int kStressMixedChains = 100;  ///< 混合成败的链数（一半被拒绝）。

/// 业务拒绝码（从 kBusinessBase 起取）。
enum
{
    kStockReject = common::async::kBusinessBase
};

// ==================== 共享脚手架 ====================

// 共享探针（CStepProbe）、轨迹（CTraceSink）与可配置被调模块（CCalleeCtx/CCalleeModule）
// 见 Tests/AsyncTestKit.h（与基础/亲和性用例共用同一套观测点）。
using asynctest::CCalleeCtx;
using asynctest::CCalleeModule;
using asynctest::CStepProbe;
using asynctest::CTraceSink;
using asynctest::EnterOrderStep;
using asynctest::EnterStockStep;
using asynctest::LeaveOrderStep;
using asynctest::LeaveStockStep;
using asynctest::SleepMs;

// ==================== 订单模块（调用方，1 线程） ====================

/// @brief 订单流程上下文（每条链一个）。
struct CStressOrderCtx
{
    int nSku;                             ///< 入参：商品号。
    int nStock;                           ///< 出参：带回来的库存。
    int nStockDelayMs;                    ///< 被调模块每步模拟耗时。
    bool bRejectStock;                    ///< 让库存模块拒绝本次查询。
    int nRounds;                          ///< 已完成的往返轮数。
    int nOwnSteps;                        ///< 本模块自有步骤数（深链用例等于层数）。
    int nResumeOnStockThread;             ///< 落在结算线程的次数（亲和后恒为 0）。
    int nResumeOnOwnThread;               ///< 回本模块线程的次数。
    std::atomic<int> nBranchDone;         ///< 已完成的分支数。
    std::atomic<int> nBranchFail;         ///< 失败的分支数。
    bool bCaught;                         ///< catch 是否执行过。
    int nCaughtCode;                      ///< catch 收到的码。
    std::shared_ptr<CTraceSink> spTrace;  ///< 本链轨迹。
    std::shared_ptr<CStepProbe> pProbe;   ///< 共享探针。
    std::thread::id idFirst;              ///< 本模块首层所在线程。
    std::thread::id idStockThread;        ///< 被调模块线程（OnSettled 回调所在）。
    std::thread::id idAfterBridge;        ///< 跨模块返回后那一层所在线程。
    std::thread::id idBackHome;           ///< 回到本模块线程后那一层所在线程。
    std::thread::id idLastOwn;            ///< 最后一个自有步骤所在线程。

    CStressOrderCtx()
        : nSku(0),
          nStock(0),
          nStockDelayMs(0),
          bRejectStock(false),
          nRounds(0),
          nOwnSteps(0),
          nResumeOnStockThread(0),
          nResumeOnOwnThread(0),
          nBranchDone(0),
          nBranchFail(0),
          bCaught(false),
          nCaughtCode(0)
    {}
};

/// @brief 订单模块：自持 1 线程执行器；跨模块调用库存模块（不传执行器）。
class CStressOrderModule
{
public:
    CStressOrderModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 单轮：A1 → 等库存（B1/B2）→ A2 → 回本模块 → A3。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spStockModule 库存模块。
    ///
    /// @return 指向最后一层的 promise。
    common::async::CPromise<CStressOrderCtx> RunOnceAsync(
        const std::shared_ptr<CStressOrderCtx>& spCtx, const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        return m_exec.NewPromise(spCtx, &StepOrderLoad, ASYNC_LOC)
            .ThenPromise(MakeQueryStockFactory(spStockModule), ASYNC_LOC)
            .Then(&StepOrderAfterBridge, ASYNC_LOC)
            .ThenPromise(MakeBackHomeFactory(), ASYNC_LOC)
            .Then(&StepOrderBackHome, ASYNC_LOC)
            .Catch(&StepOrderCatch, ASYNC_LOC);
    }

    /// @brief 多轮往返：单条链里 A↔B 往返 nRounds 次。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spStockModule 库存模块。
    /// @param nRounds 往返轮数。
    ///
    /// @return 指向最后一层的 promise。
    common::async::CPromise<CStressOrderCtx> RunRoundsAsync(
        const std::shared_ptr<CStressOrderCtx>& spCtx, const std::shared_ptr<CCalleeModule>& spStockModule, int nRounds)
    {
        common::async::CPromise<CStressOrderCtx> promise = m_exec.NewPromise(spCtx, &StepOrderLoad, ASYNC_LOC);
        for (int i = 0; i < nRounds; ++i)
        {
            promise = promise.ThenPromise(MakeQueryStockFactory(spStockModule), ASYNC_LOC)
                          .Then(&StepOrderAfterBridge, ASYNC_LOC)
                          .ThenPromise(MakeBackHomeFactory(), ASYNC_LOC)
                          .Then(&StepOrderRound, ASYNC_LOC);
        }
        return promise.Catch(&StepOrderCatch, ASYNC_LOC);
    }

    /// @brief 深链：单模块单线程，一条链 nLayers 层（压内联级联深度限制）。
    ///
    /// @param spCtx 本流程上下文。
    /// @param nLayers 层数。
    ///
    /// @return 指向最后一层的 promise。
    common::async::CPromise<CStressOrderCtx> RunDeepAsync(const std::shared_ptr<CStressOrderCtx>& spCtx, int nLayers)
    {
        common::async::CPromise<CStressOrderCtx> promise = m_exec.NewPromise(spCtx, &StepOrderLoad, ASYNC_LOC);
        for (int i = 1; i < nLayers; ++i)
        {
            promise = promise.Then(&StepOrderLeaf, ASYNC_LOC);
        }
        return promise;
    }

    /// @brief 在已落定的链上补挂一层（验证「层已 settle 后再挂 → 投递回本链执行器」）。
    ///
    /// @param promise 已落定的层句柄。
    ///
    /// @return 新层的 promise。
    common::async::CPromise<CStressOrderCtx> AppendLateAsync(common::async::CPromise<CStressOrderCtx> promise)
    {
        return promise.Then(&StepOrderLeaf, ASYNC_LOC);
    }

    /// @brief 分叉 + 汇聚：一层里发起 nBranches 个跨模块调用，全部完成才继续（手写 when_all）。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spStockModule 库存模块。
    /// @param nBranches 分支数。
    ///
    /// @return 指向最后一层的 promise。
    common::async::CPromise<CStressOrderCtx> RunFanOutAsync(const std::shared_ptr<CStressOrderCtx>& spCtx,
        const std::shared_ptr<CCalleeModule>& spStockModule, int nBranches)
    {
        return m_exec.NewPromise(spCtx, &StepOrderLoad, ASYNC_LOC)
            .ThenPromise(MakeJoinFactory(spStockModule, nBranches), ASYNC_LOC)
            .Then(&StepOrderAfterBridge, ASYNC_LOC)
            .Catch(&StepOrderCatch, ASYNC_LOC);
    }

    /// @brief 半路停掉被调模块：A1 → 停库存模块 → 再调它（应退化为 kStopped 拒绝）。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spStockModule 库存模块。
    ///
    /// @return 指向最后一层的 promise。
    common::async::CPromise<CStressOrderCtx> RunStopMidFlightAsync(
        const std::shared_ptr<CStressOrderCtx>& spCtx, const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        common::async::CPromise<CStressOrderCtx>::ThenHandler fnStopStockModule =
            [spStockModule](common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CStressOrderCtx>& spSelf)
        {
            spStockModule->Stop();  // 被调模块执行器先停：后续投递都会被拒绝。
            if (spSelf->spTrace != nullptr)
            {
                spSelf->spTrace->Append("停库存");
            }
            return common::async::CPromiseResult::Resolve();
        };

        return m_exec.NewPromise(spCtx, &StepOrderLoad, ASYNC_LOC)
            .Then(fnStopStockModule, ASYNC_LOC)
            .ThenPromise(MakeQueryStockFactory(spStockModule), ASYNC_LOC)
            .Then(&StepOrderAfterBridge, ASYNC_LOC)
            .Catch(&StepOrderCatch, ASYNC_LOC);
    }

private:
    /// 跨模块那一层的工厂：等库存模块的 promise。
    common::async::CPromise<CStressOrderCtx>::PromiseFactory MakeQueryStockFactory(
        const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        return [this, spStockModule](const std::shared_ptr<CStressOrderCtx>& spSelf)
        {
            return BridgeQueryStock(spSelf, spStockModule);
        };
    }

    /// 回到本模块线程那一层的工厂。
    common::async::CPromise<CStressOrderCtx>::PromiseFactory MakeBackHomeFactory()
    {
        return [this](const std::shared_ptr<CStressOrderCtx>& spSelf)
        {
            return PostBackToOwnThread(spSelf);
        };
    }

    /// 分叉 + 汇聚那一层的工厂。
    common::async::CPromise<CStressOrderCtx>::PromiseFactory MakeJoinFactory(
        const std::shared_ptr<CCalleeModule>& spStockModule, int nBranches)
    {
        return [this, spStockModule, nBranches](const std::shared_ptr<CStressOrderCtx>& spSelf)
        {
            return JoinBranches(spSelf, spStockModule, nBranches);
        };
    }

    /// ① 本模块自有步骤：本模块执行器线程。
    static common::async::CPromiseResult StepOrderLoad(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CStressOrderCtx>& spCtx)
    {
        EnterOrderStep(spCtx->pProbe);
        spCtx->idFirst = std::this_thread::get_id();
        spCtx->idLastOwn = spCtx->idFirst;
        ++spCtx->nOwnSteps;
        if (spCtx->spTrace != nullptr)
        {
            spCtx->spTrace->Append("A1");
        }
        LeaveOrderStep(spCtx->pProbe);
        return common::async::CPromiseResult::Resolve();
    }

    /// ④ 跨模块返回后的层：线程亲和把它拉回本模块执行器线程（本测试记录实际落点做校验）。
    static common::async::CPromiseResult StepOrderAfterBridge(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CStressOrderCtx>& spCtx)
    {
        spCtx->idAfterBridge = std::this_thread::get_id();
        if (spCtx->idAfterBridge == spCtx->idFirst)
        {
            ++spCtx->nResumeOnOwnThread;
        }
        else
        {
            ++spCtx->nResumeOnStockThread;
        }
        if (spCtx->spTrace != nullptr)
        {
            spCtx->spTrace->Append("A2");
        }
        return common::async::CPromiseResult::Resolve();
    }

    /// ⑥ 回到本模块线程后的层：应为本模块执行器线程。
    static common::async::CPromiseResult StepOrderBackHome(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CStressOrderCtx>& spCtx)
    {
        EnterOrderStep(spCtx->pProbe);
        spCtx->idBackHome = std::this_thread::get_id();
        spCtx->idLastOwn = spCtx->idBackHome;
        ++spCtx->nOwnSteps;
        if (spCtx->spTrace != nullptr)
        {
            spCtx->spTrace->Append("A3");
        }
        LeaveOrderStep(spCtx->pProbe);
        return common::async::CPromiseResult::Resolve();
    }

    /// 多轮用例的轮次层：本模块执行器线程。
    static common::async::CPromiseResult StepOrderRound(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CStressOrderCtx>& spCtx)
    {
        EnterOrderStep(spCtx->pProbe);
        ++spCtx->nRounds;
        ++spCtx->nOwnSteps;
        spCtx->idLastOwn = std::this_thread::get_id();
        if (spCtx->spTrace != nullptr)
        {
            spCtx->spTrace->Append("A3");
        }
        LeaveOrderStep(spCtx->pProbe);
        return common::async::CPromiseResult::Resolve();
    }

    /// 深链用例的叶子层：只计数（不记轨迹，避免上万次字符串追加拖慢测试）。
    static common::async::CPromiseResult StepOrderLeaf(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CStressOrderCtx>& spCtx)
    {
        EnterOrderStep(spCtx->pProbe);
        ++spCtx->nOwnSteps;
        spCtx->idLastOwn = std::this_thread::get_id();
        LeaveOrderStep(spCtx->pProbe);
        return common::async::CPromiseResult::Resolve();
    }

    /// catch 层：记录被拒绝的码。
    static common::async::CPromiseResult StepOrderCatch(
        common::async::CPromiseResult upResult, const std::shared_ptr<CStressOrderCtx>& spCtx)
    {
        spCtx->bCaught = true;
        spCtx->nCaughtCode = upResult.Code();
        if (spCtx->spTrace != nullptr)
        {
            spCtx->spTrace->Append("C");
        }
        return upResult;
    }

    /// 跨模块桥接层：本层属于本模块，被调模块在自己执行器上跑。
    common::async::CPromise<CStressOrderCtx> BridgeQueryStock(
        const std::shared_ptr<CStressOrderCtx>& spCtx, const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        common::async::CPromise<CStressOrderCtx>::PromiseExecutor fnExecutor =
            [spStockModule, spCtx](const common::async::CPromise<CStressOrderCtx>::ResolveFn& fnResolve,
                const common::async::CPromise<CStressOrderCtx>::RejectFn& fnReject)
        {
            auto spStock = std::make_shared<CCalleeCtx>();
            spStock->nSku = spCtx->nSku;
            spStock->nDelayMs = spCtx->nStockDelayMs;
            spStock->bReject = spCtx->bRejectStock;
            spStock->spTrace = spCtx->spTrace;
            spStock->pProbe = spCtx->pProbe;

            common::async::CPromise<CCalleeCtx> promiseStock = spStockModule->QueryStockAsync(spStock);
            const bool bOk = promiseStock.OnSettled(
                [spCtx, spStock, fnResolve, fnReject](common::async::CPromiseResult result)
                {
                    // 本回调要么在库存模块线程上内联跑，要么（补登记时）在库存模块执行器上跑。
                    spCtx->idStockThread = std::this_thread::get_id();
                    if (result.IsRejected())
                    {
                        fnReject(result.Code());
                        return;
                    }
                    spCtx->nStock = spStock->nAvail;
                    fnResolve();
                });
            if (!bOk)
            {
                // 子 promise 已 settled 且对方执行器不可用：回调不会执行，本层必须以拒绝收口
                // （否则本层永久 pending → 上层 Await 死等）。
                fnReject(common::async::kStopped);
            }
        };
        return m_exec.NewPromise(spCtx, fnExecutor, ASYNC_LOC);
    }

    /// 回到本模块线程：跨模块回调里显式投递到本模块执行器再 settle。
    common::async::CPromise<CStressOrderCtx> PostBackToOwnThread(const std::shared_ptr<CStressOrderCtx>& spCtx)
    {
        common::async::CPromise<CStressOrderCtx>::PromiseExecutor fnExecutor =
            [this](const common::async::CPromise<CStressOrderCtx>::ResolveFn& fnResolve,
                const common::async::CPromise<CStressOrderCtx>::RejectFn& fnReject)
        {
            if (!m_exec.Post(
                    [fnResolve]()
                    {
                        fnResolve();
                    }))
            {
                fnReject(common::async::kStopped);
            }
        };
        return m_exec.NewPromise(spCtx, fnExecutor, ASYNC_LOC);
    }

    /// 分叉 + 汇聚：发起 nBranches 条跨模块分支，全部 settle 后 settle 本层（手写 when_all）。
    common::async::CPromise<CStressOrderCtx> JoinBranches(const std::shared_ptr<CStressOrderCtx>& spCtx,
        const std::shared_ptr<CCalleeModule>& spStockModule, int nBranches)
    {
        common::async::CPromise<CStressOrderCtx>::PromiseExecutor fnExecutor =
            [spStockModule, spCtx, nBranches](const common::async::CPromise<CStressOrderCtx>::ResolveFn& fnResolve,
                const common::async::CPromise<CStressOrderCtx>::RejectFn& fnReject)
        {
            // 剩余分支计数：归零时才 settle 本层。
            auto pRemain = std::make_shared<std::atomic<int> >(nBranches);
            for (int i = 0; i < nBranches; ++i)
            {
                auto spStock = std::make_shared<CCalleeCtx>();
                spStock->nSku = spCtx->nSku + i;
                spStock->pProbe = spCtx->pProbe;

                common::async::CPromise<CCalleeCtx> promiseStock = spStockModule->QueryStockAsync(spStock);
                const bool bOk = promiseStock.OnSettled(
                    [spCtx, pRemain, fnResolve, fnReject](common::async::CPromiseResult result)
                    {
                        // 本回调在库存模块线程上；单个 worker ⇒ 不会并发进入。
                        spCtx->idStockThread = std::this_thread::get_id();
                        if (result.IsRejected())
                        {
                            ++spCtx->nBranchFail;
                        }
                        else
                        {
                            ++spCtx->nBranchDone;
                        }
                        if (--(*pRemain) == 0)
                        {
                            if (spCtx->nBranchFail.load() > 0)
                            {
                                fnReject(kStockReject);
                                return;
                            }
                            fnResolve();
                        }
                    });
                if (!bOk)
                {
                    // 分支已 settled 但对方执行器不可用：计失败并计入剩余数（不能永久 pending）。
                    ++spCtx->nBranchFail;
                    if (--(*pRemain) == 0)
                    {
                        fnReject(common::async::kStopped);
                    }
                }
            }
        };
        return m_exec.NewPromise(spCtx, fnExecutor, ASYNC_LOC);
    }

    common::async::CAsyncExecutor m_exec;  ///< 模块私有执行器（单线程）。
};

// ==================== 工具 ====================

/// @brief 建一个带轨迹与探针的订单上下文。
static std::shared_ptr<CStressOrderCtx> MakeOrderCtx(const std::shared_ptr<CStepProbe>& pProbe, int nSku)
{
    auto spCtx = std::make_shared<CStressOrderCtx>();
    spCtx->nSku = nSku;
    spCtx->pProbe = pProbe;
    spCtx->spTrace = std::make_shared<CTraceSink>();
    return spCtx;
}

// ==================== 用例 ====================

/// @brief 极限 1：200 条链同时跨模块调用一个单线程模块。
TEST(ModuleStress_ManyConcurrentChains)
{
    const std::thread::id idMain = std::this_thread::get_id();

    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<CStressOrderModule>();
    auto spProbe = std::make_shared<CStepProbe>();

    std::vector<std::shared_ptr<CStressOrderCtx> > vecCtx;
    std::vector<common::async::CPromise<CStressOrderCtx> > vecPromise;
    for (int i = 0; i < kStressChains; ++i)
    {
        auto spCtx = MakeOrderCtx(spProbe, i);
        vecCtx.push_back(spCtx);
        vecPromise.push_back(spOrderModule->RunOnceAsync(spCtx, spStockModule));
    }

    for (int i = 0; i < kStressChains; ++i)
    {
        ASSERT_TRUE(vecPromise[i].Await().IsFulfilled());
    }

    for (int i = 0; i < kStressChains; ++i)
    {
        // 每条链自身顺序完整（200 条并发也不交错、不丢步）
        ASSERT_EQ(vecCtx[i]->spTrace->strTrace, std::string("A1;B1;B2;A2;A3;"));
        ASSERT_EQ(vecCtx[i]->nStock, 5);
        ASSERT_EQ(vecCtx[i]->nOwnSteps, 2);
        ASSERT_EQ(vecCtx[i]->spTrace->Count("B2"), 1);

        // 线程：A1/A3 必在订单模块线程（自己的执行器）
        ASSERT_TRUE(vecCtx[i]->idFirst == vecCtx[i]->idBackHome);
        ASSERT_TRUE(vecCtx[i]->idFirst != idMain);

        // 跨模块返回后那一层：线程亲和保证它恒在**本模块执行器线程**上
        ASSERT_TRUE(vecCtx[i]->idAfterBridge == vecCtx[i]->idFirst);
        ASSERT_TRUE(vecCtx[i]->idAfterBridge != idMain);
        ASSERT_EQ(vecCtx[i]->nResumeOnStockThread, 0);
        ASSERT_EQ(vecCtx[i]->nResumeOnOwnThread, 1);

        // 跨链：模块线程固定（每模块只有 1 个 worker）
        ASSERT_TRUE(vecCtx[i]->idFirst == vecCtx[0]->idFirst);
        ASSERT_TRUE(vecCtx[i]->idStockThread == vecCtx[0]->idStockThread);
    }

    // 两种续跑线程都会出现（取决于「挂下一层」是否早于上一层 settle），这里只统计分布
    int nResumeOnStock = 0;
    int nResumeOnOwn = 0;
    for (int i = 0; i < kStressChains; ++i)
    {
        nResumeOnStock += vecCtx[i]->nResumeOnStockThread;
        nResumeOnOwn += vecCtx[i]->nResumeOnOwnThread;
    }
    // 线程亲和：200 条链的续跑全部落在本模块执行器线程上（修复前实测 57:143 二选一）
    std::printf(
        "      %d 条链：续跑在本模块线程 %d 条 / 在结算线程 %d 条\n", kStressChains, nResumeOnOwn, nResumeOnStock);
    ASSERT_EQ(nResumeOnOwn, kStressChains);
    ASSERT_EQ(nResumeOnStock, 0);

    // 模块内不重叠 + 步数精确
    ASSERT_EQ(spProbe->nOrderMaxInFlight.load(), 1);
    ASSERT_EQ(spProbe->nStockMaxInFlight.load(), 1);
    ASSERT_EQ(spProbe->nOrderSteps.load(), kStressChains * 2);
    ASSERT_EQ(spProbe->nStockSteps.load(), kStressChains * 2);
}

/// @brief 极限 2：单条链 100 轮 A↔B 往返（跨模块 + 回本模块交替）。
TEST(ModuleStress_DeepRoundTrips)
{
    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<CStressOrderModule>();
    auto spProbe = std::make_shared<CStepProbe>();
    auto spCtx = MakeOrderCtx(spProbe, 7);
    spCtx->nStockDelayMs = 2;  // 库存每步 2ms：保证「挂下一层」早于 settle

    ASSERT_TRUE(spOrderModule->RunRoundsAsync(spCtx, spStockModule, kStressRounds).Await().IsFulfilled());

    // 顺序：A1 之后每轮 "B1;B2;A2;A3;"，共 kStressRounds 轮
    std::string strExpected = "A1;";
    for (int i = 0; i < kStressRounds; ++i)
    {
        strExpected += "B1;B2;A2;A3;";
    }
    ASSERT_EQ(spCtx->spTrace->strTrace, strExpected);
    ASSERT_EQ(spCtx->nRounds, kStressRounds);
    ASSERT_EQ(spCtx->nOwnSteps, kStressRounds + 1);

    // 线程亲和：每轮的 A2 都回到订单模块自己的执行器线程（即使挂层早于 settle）；A3 同线程
    ASSERT_EQ(spCtx->nResumeOnOwnThread, kStressRounds);
    ASSERT_EQ(spCtx->nResumeOnStockThread, 0);
    ASSERT_TRUE(spCtx->idAfterBridge == spCtx->idFirst);
    ASSERT_TRUE(spCtx->idAfterBridge != spCtx->idStockThread);
    ASSERT_TRUE(spCtx->idLastOwn == spCtx->idFirst);

    // 模块内串行、无重叠
    ASSERT_EQ(spProbe->nOrderMaxInFlight.load(), 1);
    ASSERT_EQ(spProbe->nStockMaxInFlight.load(), 1);
    ASSERT_EQ(spProbe->nStockSteps.load(), kStressRounds * 2);
    ASSERT_EQ(spProbe->nOrderSteps.load(), kStressRounds + 1);
}

/// @brief 极限 3：单条链 5000 层（单线程执行器全速级联，压防爆栈上限）。
TEST(ModuleStress_DeepSingleChain)
{
    auto spOrderModule = std::make_shared<CStressOrderModule>();
    auto spProbe = std::make_shared<CStepProbe>();
    auto spCtx = MakeOrderCtx(spProbe, 1);

    ASSERT_TRUE(spOrderModule->RunDeepAsync(spCtx, kStressDeepLayers).Await().IsFulfilled());

    ASSERT_EQ(spCtx->nOwnSteps, kStressDeepLayers);
    ASSERT_EQ(spProbe->nOrderSteps.load(), kStressDeepLayers);
    ASSERT_EQ(spProbe->nOrderMaxInFlight.load(), 1);
    ASSERT_TRUE(spCtx->idLastOwn == spCtx->idFirst);
}

/// @brief 极限 4：8 个线程同时 Await 同一条跨模块链（链只跑一遍）。
TEST(ModuleStress_ConcurrentAwaitSameChain)
{
    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<CStressOrderModule>();
    auto spProbe = std::make_shared<CStepProbe>();
    auto spCtx = MakeOrderCtx(spProbe, 3);

    const common::async::CPromise<CStressOrderCtx> promise = spOrderModule->RunOnceAsync(spCtx, spStockModule);

    std::atomic<int> nFulfilled(0);
    std::atomic<int> nRejected(0);
    std::vector<std::thread> vecWaiter;
    for (int i = 0; i < kStressWaiters; ++i)
    {
        vecWaiter.push_back(std::thread(
            [promise, &nFulfilled, &nRejected]()
            {
                if (promise.Await().IsFulfilled())
                {
                    ++nFulfilled;
                }
                else
                {
                    ++nRejected;
                }
            }));
    }
    for (size_t i = 0; i < vecWaiter.size(); ++i)
    {
        vecWaiter[i].join();
    }

    ASSERT_EQ(nFulfilled.load(), kStressWaiters);
    ASSERT_EQ(nRejected.load(), 0);

    // 链只跑一遍：A1/A3 各一次、库存两步各一次
    ASSERT_EQ(spCtx->nOwnSteps, 2);
    ASSERT_EQ(spProbe->nStockSteps.load(), 2);
    ASSERT_EQ(spCtx->spTrace->strTrace, std::string("A1;B1;B2;A2;A3;"));
}

/// @brief 极限 5：一层分叉 64 条跨模块分支 + 手写汇聚（等价 when_all）。
TEST(ModuleStress_FanOutJoin)
{
    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<CStressOrderModule>();
    auto spProbe = std::make_shared<CStepProbe>();
    auto spCtx = MakeOrderCtx(spProbe, 10);

    ASSERT_TRUE(spOrderModule->RunFanOutAsync(spCtx, spStockModule, kStressBranches).Await().IsFulfilled());

    // 64 条分支全部完成、无失败；库存模块串行处理（单 worker）
    ASSERT_EQ(spCtx->nBranchDone.load(), kStressBranches);
    ASSERT_EQ(spCtx->nBranchFail.load(), 0);
    ASSERT_EQ(spProbe->nStockSteps.load(), kStressBranches * 2);
    ASSERT_EQ(spProbe->nStockMaxInFlight.load(), 1);
    ASSERT_EQ(spProbe->nOrderMaxInFlight.load(), 1);

    // 汇聚层那一层：线程亲和保证它恒在本模块执行器线程上
    ASSERT_TRUE(spCtx->idAfterBridge == spCtx->idFirst);
    ASSERT_EQ(spCtx->nResumeOnOwnThread, 1);
    ASSERT_EQ(spCtx->nResumeOnStockThread, 0);
}

/// @brief 极限 8：层已 settled 后再挂下一层 → 投递回本链执行器（本模块线程）执行。
TEST(ModuleStress_LateAppendRunsOnOwnExecutor)
{
    const std::thread::id idMain = std::this_thread::get_id();

    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<CStressOrderModule>();
    auto spProbe = std::make_shared<CStepProbe>();
    auto spCtx = MakeOrderCtx(spProbe, 13);

    const common::async::CPromise<CStressOrderCtx> promise = spOrderModule->RunOnceAsync(spCtx, spStockModule);
    ASSERT_TRUE(promise.Await().IsFulfilled());
    const int nStepsBefore = spCtx->nOwnSteps;

    // 补登记：本层已 settled，新层只能投递到本链执行器执行 → 跑在本模块线程
    ASSERT_TRUE(spOrderModule->AppendLateAsync(promise).Await().IsFulfilled());

    ASSERT_EQ(spCtx->nOwnSteps, nStepsBefore + 1);
    ASSERT_TRUE(spCtx->idLastOwn == spCtx->idFirst);
    ASSERT_TRUE(spCtx->idLastOwn != idMain);
}

/// @brief 极限 6：100 条链一半让被调模块拒绝（成功与失败互不串）。
TEST(ModuleStress_MixedRejections)
{
    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<CStressOrderModule>();
    auto spProbe = std::make_shared<CStepProbe>();

    std::vector<std::shared_ptr<CStressOrderCtx> > vecCtx;
    std::vector<common::async::CPromise<CStressOrderCtx> > vecPromise;
    for (int i = 0; i < kStressMixedChains; ++i)
    {
        auto spCtx = MakeOrderCtx(spProbe, i);
        spCtx->bRejectStock = (i % 2 == 1);  // 奇数链走拒绝分支
        vecCtx.push_back(spCtx);
        vecPromise.push_back(spOrderModule->RunOnceAsync(spCtx, spStockModule));
    }

    for (int i = 0; i < kStressMixedChains; ++i)
    {
        const common::async::CPromiseResult result = vecPromise[i].Await();
        if (i % 2 == 1)
        {
            // 被调模块拒绝 → 本链以同一码拒绝，后续层全跳过，catch 执行
            ASSERT_TRUE(result.IsRejected());
            ASSERT_EQ(result.Code(), kStockReject);
            ASSERT_TRUE(vecCtx[i]->bCaught);
            ASSERT_EQ(vecCtx[i]->nCaughtCode, kStockReject);
            ASSERT_EQ(vecCtx[i]->nOwnSteps, 1);  // 只有 A1 执行过
            ASSERT_EQ(vecCtx[i]->spTrace->strTrace, std::string("A1;B1;B2;C;"));
        }
        else
        {
            ASSERT_TRUE(result.IsFulfilled());
            ASSERT_EQ(vecCtx[i]->nStock, 5);
            ASSERT_EQ(vecCtx[i]->nOwnSteps, 2);
            ASSERT_TRUE(vecCtx[i]->bCaught == false);
        }
    }

    ASSERT_EQ(spProbe->nStockSteps.load(), kStressMixedChains * 2);
    ASSERT_EQ(spProbe->nOrderMaxInFlight.load(), 1);
    ASSERT_EQ(spProbe->nStockMaxInFlight.load(), 1);
}

/// @brief 极限 7：链跑到一半停掉被调模块执行器（退化为 kStopped，不挂死）。
TEST(ModuleStress_StopMidFlight)
{
    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<CStressOrderModule>();
    auto spProbe = std::make_shared<CStepProbe>();
    auto spCtx = MakeOrderCtx(spProbe, 5);

    const common::async::CPromiseResult result = spOrderModule->RunStopMidFlightAsync(spCtx, spStockModule).Await();

    // 被调模块已停止：投递失败 → 本链以 kStopped 拒绝，后续步骤全跳过
    ASSERT_TRUE(result.IsRejected());
    ASSERT_EQ(result.Code(), common::async::kStopped);
    ASSERT_TRUE(spCtx->bCaught);
    ASSERT_EQ(spCtx->nCaughtCode, common::async::kStopped);
    ASSERT_EQ(spCtx->nOwnSteps, 1);                                    // 只有 A1 执行过
    ASSERT_EQ(spProbe->nStockSteps.load(), 0);                         // 库存模块一步都没跑
    ASSERT_EQ(spCtx->spTrace->strTrace, std::string("A1;停库存;C;"));  // 后续步骤全跳过，catch 收尾
}
