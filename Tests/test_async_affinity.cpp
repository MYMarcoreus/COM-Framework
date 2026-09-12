/// @file test_async_affinity.cpp
/// 改进 A（线程亲和）的专项测试。
///
/// 承诺（本文件即验收标准）：**每一层都在它所属链的执行器线程上执行**。
///  - 本模块的层：始终在自己模块的执行器线程上（同执行器内联，线程不变）；
///  - 跨模块返回后那一层：被调模块 settle 本链时，本层**回到本模块执行器线程**
///    （修复前是"二选一"：约一半的续跑落在被调模块线程上）；
///  - 协程：跨模块 await 之后协程体也在本模块执行器线程上续跑；
///  - 边界：`OnSettled` 是"通知"不是"层"，仍在结算线程上触发（不迁移）；
///  - 代价与安全：每次跨模块返回多一次入队；单线程执行器不会因此死锁、模块内仍不重叠。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "AsyncTestKit.h"
#include "TestFramework.h"

namespace no = common::async;

// 共享脚手架见 Tests/AsyncTestKit.h：被调模块（CCalleeCtx/CCalleeModule，自持 1 线程、
// 两步）与观测点（CStepProbe 的步数/并发峰值）。本文件不依赖轨迹，故不接 CTraceSink。
using asynctest::CCalleeCtx;
using asynctest::CCalleeModule;
using asynctest::CStepProbe;

// ==================== 订单模块（调用方，1 线程） ====================

/// @brief 订单流程上下文。
struct CAffinityOrderCtx
{
    int nSku;                       ///< 入参：商品号。
    int nStock;                     ///< 出参：带回来的库存。
    int nStockDelayMs;              ///< 让被调模块每步耗时（覆盖"登记早于 settle"路径）。
    int nOwnSteps;                  ///< 本模块自有步骤数。
    int nRounds;                    ///< 已完成往返轮数。
    int nResumeOnOwnThread;         ///< 跨模块返回后在本模块线程续跑的次数。
    int nResumeOnStockThread;       ///< 跨模块返回后落在被调模块线程的次数（应为 0）。
    std::thread::id idFirst;        ///< 本模块首层所在线程。
    std::thread::id idAfterBridge;  ///< 跨模块返回后那一层所在线程。
    std::thread::id idBackHome;     ///< 显式 Post 回本模块后那一层所在线程。
    std::thread::id idLastOwn;      ///< 最后一个自有步骤所在线程。
    std::thread::id idOnSettled;    ///< 被调 promise 的 OnSettled 通知所在线程。

    CAffinityOrderCtx()
        : nSku(0), nStock(0), nStockDelayMs(0), nOwnSteps(0), nRounds(0), nResumeOnOwnThread(0), nResumeOnStockThread(0)
    {}
};

/// @brief 订单模块：自持 1 线程执行器；跨模块调用库存模块（不传执行器）。
class CAffinityOrderModule
{
public:
    class CAffinityFlowCoro;  ///< 前向声明（RunCoAsync 的返回类型需要；定义见下）。

    CAffinityOrderModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 单轮：A1 → 等库存 → A2（跨模块返回）→ 显式 Post 回本模块 → A3。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spStockModule 库存模块。
    ///
    /// @return 指向最后一层的 promise。
    no::CPromise<CAffinityOrderCtx> RunOnceAsync(
        const std::shared_ptr<CAffinityOrderCtx>& spCtx, const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        return m_exec.NewPromise(spCtx, &StepOrderLoad, ASYNC_LOC)
            .ThenPromise(MakeQueryFactory(spStockModule), ASYNC_LOC)
            .Then(&StepOrderAfterBridge, ASYNC_LOC)
            .ThenPromise(MakeBackHomeFactory(), ASYNC_LOC)
            .Then(&StepOrderBackHome, ASYNC_LOC);
    }

    /// @brief 多轮往返：A↔B 往返 nRounds 次（每轮的 A2 都应在回到本模块线程）。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spStockModule 库存模块。
    /// @param nRounds 往返轮数。
    ///
    /// @return 指向最后一层的 promise。
    no::CPromise<CAffinityOrderCtx> RunRoundsAsync(const std::shared_ptr<CAffinityOrderCtx>& spCtx,
        const std::shared_ptr<CCalleeModule>& spStockModule, int nRounds)
    {
        no::CPromise<CAffinityOrderCtx> promise = m_exec.NewPromise(spCtx, &StepOrderLoad, ASYNC_LOC);
        for (int i = 0; i < nRounds; ++i)
        {
            promise = promise.ThenPromise(MakeQueryFactory(spStockModule), ASYNC_LOC)
                          .Then(&StepOrderAfterBridge, ASYNC_LOC)
                          .Then(&StepOrderRound, ASYNC_LOC);
        }
        return promise;
    }

    /// @brief 走一次跨模块调用并给被调 promise 挂 OnSettled 通知（观察通知线程）。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spStockModule 库存模块。
    ///
    /// @return 指向最后一层的 promise。
    no::CPromise<CAffinityOrderCtx> RunWithOnSettledAsync(
        const std::shared_ptr<CAffinityOrderCtx>& spCtx, const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        return m_exec.NewPromise(spCtx, &StepOrderLoad, ASYNC_LOC)
            .ThenPromise(MakeQueryFactory(spStockModule), ASYNC_LOC)
            .Then(&StepOrderAfterBridge, ASYNC_LOC);
    }

    /// @brief 协程版：跨模块 await 之后，协程体应回到本模块执行器线程。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spStockModule 库存模块。
    /// @param promiseStock 要 await 的跨模块 promise（启动前注入）。
    ///
    /// @return 协程句柄（Await 取结果）。
    std::shared_ptr<CAffinityFlowCoro> RunCoAsync(const std::shared_ptr<CAffinityOrderCtx>& spCtx,
        const std::shared_ptr<CCalleeModule>& spStockModule, const no::CPromise<CCalleeCtx>& promiseStock)
    {
        return m_exec.CoStart<CAffinityFlowCoro>(spCtx, spStockModule, promiseStock);
    }

    /// @brief 跨模块协程：先在本模块跑一层，await 别的模块的 promise，再回到本模块跑一层。
    class CAffinityFlowCoro : public no::CCoroutine<CAffinityOrderCtx>
    {
    public:
        /// @brief 创建协程。
        ///
        /// @param spCtx 本流程上下文。
        /// @param spStockModule 库存模块（跨模块 await 用，需保活）。
        /// @param promiseStock 要 await 的跨模块 promise。
        CAffinityFlowCoro(const std::shared_ptr<CAffinityOrderCtx>& spCtx,
            const std::shared_ptr<CCalleeModule>& spStockModule, const no::CPromise<CCalleeCtx>& promiseStock)
            : no::CCoroutine<CAffinityOrderCtx>(spCtx), m_pStock(promiseStock), m_spStockModule(spStockModule)
        {}

        void Run() override
        {
            CO_BEGIN();
            CO_AWAIT(NewPromise(StepOrderLoad));  // 本模块执行器线程
            CO_AWAIT(m_pStock);                   // 跨模块：等库存模块的 promise（另一套上下文）
            CO_AWAIT(NewPromise(StepOrderAfterBridge));  // 应回到本模块执行器线程
            CO_RETURN(no::CPromiseResult::Resolve());
            CO_END();
        }

    private:
        no::CPromise<CCalleeCtx> m_pStock;               ///< 跨模块 await 的目标。
        std::shared_ptr<CCalleeModule> m_spStockModule;  ///< 库存模块（保活）。
    };

private:
    /// 跨模块那一层的工厂。
    no::CPromise<CAffinityOrderCtx>::PromiseFactory MakeQueryFactory(
        const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        return [this, spStockModule](const std::shared_ptr<CAffinityOrderCtx>& spSelf)
        {
            return BridgeQueryStock(spSelf, spStockModule);
        };
    }

    /// 回到本模块线程那一层的工厂。
    no::CPromise<CAffinityOrderCtx>::PromiseFactory MakeBackHomeFactory()
    {
        return [this](const std::shared_ptr<CAffinityOrderCtx>& spSelf)
        {
            return PostBackToOwnThread(spSelf);
        };
    }

    /// ① 本模块自有步骤。
    static no::CPromiseResult StepOrderLoad(
        no::CPromiseResult /*upResult*/, const std::shared_ptr<CAffinityOrderCtx>& spCtx)
    {
        ++spCtx->nOwnSteps;
        spCtx->idFirst = std::this_thread::get_id();
        spCtx->idLastOwn = spCtx->idFirst;
        return no::CPromiseResult::Resolve();
    }

    /// ④ 跨模块返回后的层：线程亲和应把它拉回本模块执行器线程。
    static no::CPromiseResult StepOrderAfterBridge(
        no::CPromiseResult /*upResult*/, const std::shared_ptr<CAffinityOrderCtx>& spCtx)
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
        return no::CPromiseResult::Resolve();
    }

    /// 多轮用例的轮次层。
    static no::CPromiseResult StepOrderRound(
        no::CPromiseResult /*upResult*/, const std::shared_ptr<CAffinityOrderCtx>& spCtx)
    {
        ++spCtx->nOwnSteps;
        ++spCtx->nRounds;
        spCtx->idLastOwn = std::this_thread::get_id();
        return no::CPromiseResult::Resolve();
    }

    /// ⑥ 显式 Post 回本模块后的层。
    static no::CPromiseResult StepOrderBackHome(
        no::CPromiseResult /*upResult*/, const std::shared_ptr<CAffinityOrderCtx>& spCtx)
    {
        ++spCtx->nOwnSteps;
        spCtx->idBackHome = std::this_thread::get_id();
        spCtx->idLastOwn = spCtx->idBackHome;
        return no::CPromiseResult::Resolve();
    }

    /// 跨模块桥接层（本层属于本模块；被调模块在自己执行器上跑）。
    no::CPromise<CAffinityOrderCtx> BridgeQueryStock(
        const std::shared_ptr<CAffinityOrderCtx>& spCtx, const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        no::CPromise<CAffinityOrderCtx>::PromiseExecutor fnExecutor =
            [spStockModule, spCtx](const no::CPromise<CAffinityOrderCtx>::ResolveFn& fnResolve,
                const no::CPromise<CAffinityOrderCtx>::RejectFn& fnReject)
        {
            auto spStock = std::make_shared<CCalleeCtx>();
            spStock->nDelayMs = spCtx->nStockDelayMs;

            no::CPromise<CCalleeCtx> promiseStock = spStockModule->QueryStockAsync(spStock);
            const bool bOk = promiseStock.OnSettled(
                [spCtx, spStock, fnResolve, fnReject](no::CPromiseResult result)
                {
                    // 通知（OnSettled）不迁移：本回调跑在被调模块线程上。
                    spCtx->idOnSettled = std::this_thread::get_id();
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
                fnReject(no::kStopped);
            }
        };
        return no::CPromise<CAffinityOrderCtx>::New(m_exec, spCtx, fnExecutor, ASYNC_LOC);
    }

    /// 显式投递回本模块执行器（线程亲和已保证，这里是"显式强制"的写法对照）。
    no::CPromise<CAffinityOrderCtx> PostBackToOwnThread(const std::shared_ptr<CAffinityOrderCtx>& spCtx)
    {
        no::CPromise<CAffinityOrderCtx>::PromiseExecutor fnExecutor =
            [this](const no::CPromise<CAffinityOrderCtx>::ResolveFn& fnResolve,
                const no::CPromise<CAffinityOrderCtx>::RejectFn& fnReject)
        {
            if (!m_exec.Post(
                    [fnResolve]()
                    {
                        fnResolve();
                    }))
            {
                fnReject(no::kStopped);
            }
        };
        return no::CPromise<CAffinityOrderCtx>::New(m_exec, spCtx, fnExecutor, ASYNC_LOC);
    }

    no::CAsyncExecutor m_exec;  ///< 模块私有执行器（单线程）。
};

// ==================== 用例 ====================

/// @brief 跨模块返回后那一层：恒在本模块执行器线程（被调模块 0 延迟）。
TEST(Affinity_CrossModuleReturnsToOwnExecutor)
{
    const std::thread::id idMain = std::this_thread::get_id();

    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<CAffinityOrderModule>();
    auto spCtx = std::make_shared<CAffinityOrderCtx>();
    spCtx->nSku = 7;

    ASSERT_TRUE(spOrderModule->RunOnceAsync(spCtx, spStockModule).Await().IsFulfilled());

    ASSERT_EQ(spCtx->nStock, 5);
    ASSERT_TRUE(spCtx->idAfterBridge == spCtx->idFirst);  // 回到本模块线程
    ASSERT_TRUE(spCtx->idBackHome == spCtx->idFirst);
    ASSERT_EQ(spCtx->nResumeOnOwnThread, 1);
    ASSERT_EQ(spCtx->nResumeOnStockThread, 0);
    // 通知仍在结算线程（被调模块线程）—— 迁只迁"层"，通知不迁
    ASSERT_TRUE(spCtx->idOnSettled != spCtx->idFirst);
    ASSERT_TRUE(spCtx->idFirst != idMain);
}

/// @brief 同上，但被调模块每步 5ms（走"挂下一层早于 settle"这条路径）—— 仍在本地线程。
TEST(Affinity_CrossModuleWithSlowCallee)
{
    const std::thread::id idMain = std::this_thread::get_id();

    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<CAffinityOrderModule>();
    auto spCtx = std::make_shared<CAffinityOrderCtx>();
    spCtx->nSku = 8;
    spCtx->nStockDelayMs = 5;

    ASSERT_TRUE(spOrderModule->RunOnceAsync(spCtx, spStockModule).Await().IsFulfilled());

    ASSERT_TRUE(spCtx->idAfterBridge == spCtx->idFirst);
    ASSERT_EQ(spCtx->nResumeOnOwnThread, 1);
    ASSERT_EQ(spCtx->nResumeOnStockThread, 0);
    ASSERT_TRUE(spCtx->idOnSettled != spCtx->idFirst);
    ASSERT_TRUE(spCtx->idFirst != idMain);
}

/// @brief 50 轮往返：每一轮的跨模块返回层都在本模块执行器线程上。
TEST(Affinity_AllLayersOnOwnExecutorOverRounds)
{
    const int nRounds = 50;

    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<CAffinityOrderModule>();
    auto spCtx = std::make_shared<CAffinityOrderCtx>();
    spCtx->nSku = 9;

    ASSERT_TRUE(spOrderModule->RunRoundsAsync(spCtx, spStockModule, nRounds).Await().IsFulfilled());

    ASSERT_EQ(spCtx->nRounds, nRounds);
    ASSERT_EQ(spCtx->nOwnSteps, nRounds + 1);
    ASSERT_EQ(spCtx->nResumeOnOwnThread, nRounds);
    ASSERT_EQ(spCtx->nResumeOnStockThread, 0);
    ASSERT_TRUE(spCtx->idLastOwn == spCtx->idFirst);
}

/// @brief 协程跨模块 await 之后，协程体回到本模块执行器线程。
TEST(Affinity_CoroutineResumesOnOwnExecutor)
{
    const std::thread::id idMain = std::this_thread::get_id();

    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<CAffinityOrderModule>();
    auto spCtx = std::make_shared<CAffinityOrderCtx>();
    spCtx->nSku = 10;

    // 协程要 await 的跨模块 promise（库存模块自己的上下文）。
    auto spStock = std::make_shared<CCalleeCtx>();
    auto pProbe = std::make_shared<CStepProbe>();
    spStock->pProbe = pProbe;
    const no::CPromise<CCalleeCtx> promiseStock = spStockModule->QueryStockAsync(spStock);

    std::shared_ptr<CAffinityOrderModule::CAffinityFlowCoro> pCoro =
        spOrderModule->RunCoAsync(spCtx, spStockModule, promiseStock);

    ASSERT_TRUE(pCoro->Await().IsFulfilled());

    ASSERT_EQ(spCtx->nOwnSteps, 1);                          // StepOrderLoad（A2 层不计数）
    ASSERT_TRUE(spCtx->idAfterBridge != std::thread::id());  // 协程确实跑到了跨模块之后那一层
    ASSERT_EQ(pProbe->nStockSteps.load(), 2);                // 被调模块两步
    ASSERT_EQ(pProbe->nStockMaxInFlight.load(), 1);          // 单线程模块：步骤不重叠
    ASSERT_TRUE(spCtx->idAfterBridge == spCtx->idFirst);     // 协程体回到本模块线程
    ASSERT_TRUE(spCtx->idAfterBridge != spStock->idRead);    // 不是被调模块线程
    ASSERT_TRUE(spCtx->idFirst != idMain);
}

/// @brief 4 条并发跨模块链：模块内不重叠，且跨模块返回层全在本模块线程（不死锁）。
TEST(Affinity_ConcurrentChainsOwnThreadNoOverlap)
{
    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<CAffinityOrderModule>();

    const int nChains = 4;
    std::vector<std::shared_ptr<CAffinityOrderCtx> > vecCtx;
    std::vector<no::CPromise<CAffinityOrderCtx> > vecPromise;
    for (int i = 0; i < nChains; ++i)
    {
        auto spCtx = std::make_shared<CAffinityOrderCtx>();
        spCtx->nSku = i;
        vecCtx.push_back(spCtx);
        vecPromise.push_back(spOrderModule->RunOnceAsync(spCtx, spStockModule));
    }

    for (int i = 0; i < nChains; ++i)
    {
        ASSERT_TRUE(vecPromise[i].Await().IsFulfilled());
        ASSERT_TRUE(vecCtx[i]->idAfterBridge == vecCtx[i]->idFirst);
        ASSERT_TRUE(vecCtx[i]->idFirst == vecCtx[0]->idFirst);  // 本模块线程固定
        ASSERT_EQ(vecCtx[i]->nResumeOnOwnThread, 1);
        ASSERT_EQ(vecCtx[i]->nResumeOnStockThread, 0);
    }

    // 被调模块是单线程：无论谁跨过来，内部步骤都串行不重叠
    for (size_t i = 1; i < vecCtx.size(); ++i)
    {
        ASSERT_TRUE(vecCtx[i]->idOnSettled == vecCtx[0]->idOnSettled);
    }
    std::printf("      %d 条并发链：跨模块返回层全部在本模块线程\n", nChains);
}
