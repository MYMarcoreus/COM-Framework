/// @file test_async_modules.cpp
/// 跨模块异步调用测试：每个模块自持执行器（各 1 线程），验证执行顺序与线程归属。
///
/// 关注点（对应用户约定「执行器是模块私有资源，不跨模块传递」）：
///  - 顺序：跨模块链的先后由依赖边保证（A1 → B1 → B2 → A2 → A3），与线程数无关；
///  - 线程：每个模块的步骤跑在自己的执行器线程上，模块之间线程不同、且都不是调用线程；
///  - 续跑归属：跨模块返回后的那一层由「线程亲和」拉回**本模块执行器线程**（改革前是二选一）；
///    `OnSettled` 通知仍在结算线程（= 被调模块线程）上触发；
///    要回到本模块线程需显式 `exec.Post(...)`（示例见 `PostBackToOwnThread`）；
///  - 单线程模块：模块内步骤串行、不重叠（并发调用也只是排队）。

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "AsyncTestKit.h"
#include "TestFramework.h"

namespace no = common::async;

// 共享脚手架（观测工具 + 可配置的被调模块）见 Tests/AsyncTestKit.h：
//   CTraceSink（步骤轨迹 + 每步线程）/ CStepProbe（并发与步数探针）/ CCalleeCtx、CCalleeModule（两步被调模块）
using asynctest::CCalleeCtx;
using asynctest::CCalleeModule;
using asynctest::CStepProbe;
using asynctest::CTraceSink;
using asynctest::EnterStep;
using asynctest::LeaveStep;
using asynctest::SleepMs;

// ==================== 订单模块（调用方） ====================

/// @brief 订单流程上下文。
struct COrderCtx
{
    int nSku;                             ///< 入参：商品号。
    int nStock;                           ///< 出参：从库存模块带回来的库存。
    std::shared_ptr<CTraceSink> spTrace;  ///< 轨迹（跨模块共享观测点）。
    std::thread::id idFirst;              ///< 本模块首层所在线程。
    std::thread::id idAfterBridge;        ///< 跨模块返回后那一层所在线程。
    std::thread::id idBackHome;           ///< 显式投递回本模块线程后那一层所在线程。

    COrderCtx() : nSku(0), nStock(0)
    {}
};

/// @brief 订单模块：自持 1 线程执行器；跨模块调用库存模块并等它。
class COrderModule
{
   public:
    COrderModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 下单流程：本模块 → 库存模块（等它）→ 回到本模块。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spStockModule 库存模块（只传 promise 与上下文，不传执行器）。
    ///
    /// @return 指向最后一层的 promise。
    no::CPromise<COrderCtx> PlaceOrderAsync(const std::shared_ptr<COrderCtx>& spCtx,
                                            const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        // ③ 跨模块那一层：等库存模块的 promise（桥接层属于本模块）
        no::CPromise<COrderCtx>::PromiseFactory fnQueryStock =
            [this, spStockModule](const std::shared_ptr<COrderCtx>& spSelf)
        {
            return BridgeQueryStock(spSelf, spStockModule);
        };

        // ⑤ 回到本模块线程：显式投递到本模块执行器
        no::CPromise<COrderCtx>::PromiseFactory fnBackHome = [this](const std::shared_ptr<COrderCtx>& spSelf)
        {
            return PostBackToOwnThread(spSelf);
        };

        return m_exec
            .NewPromise(spCtx, &StepLoadOrder, ASYNC_LOC)  // ① 本模块执行器
            .ThenPromise(fnQueryStock, ASYNC_LOC)          // ② + ③ 库存模块
            .Then(&StepTakeStock, ASYNC_LOC)               // ④ 线程亲和：回本模块线程
            .ThenPromise(fnBackHome, ASYNC_LOC)            // ⑤ 投递回本模块
            .Then(&StepFinish, ASYNC_LOC);                 // ⑥ 本模块执行器
    }

   private:
    /// ① 读订单：本模块执行器线程。
    static no::CPromiseResult StepLoadOrder(no::CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
    {
        spCtx->idFirst = std::this_thread::get_id();
        spCtx->spTrace->Append("A1");
        return no::CPromiseResult::Resolve();
    }

    /// ④ 跨模块返回后的层：记录本层线程（应为库存模块线程）。
    static no::CPromiseResult StepTakeStock(no::CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
    {
        spCtx->idAfterBridge = std::this_thread::get_id();
        spCtx->spTrace->Append("A2");
        return no::CPromiseResult::Resolve();
    }

    /// ⑥ 回到本模块线程后的层：记录本层线程（应为本模块执行器线程）。
    static no::CPromiseResult StepFinish(no::CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
    {
        spCtx->idBackHome = std::this_thread::get_id();
        spCtx->spTrace->Append("A3");
        return no::CPromiseResult::Resolve();
    }

    /// ③ 桥接：把库存模块的 promise 接进本流程（等价 JS `new Promise`）。
    ///
    /// 桥接层用本模块执行器创建；库存模块在自己的执行器上跑。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spStockModule 库存模块。
    ///
    /// @return 由库存模块回调 settle 的本流程 promise。
    no::CPromise<COrderCtx> BridgeQueryStock(const std::shared_ptr<COrderCtx>& spCtx,
                                             const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        no::CPromise<COrderCtx>::PromiseExecutor fnExecutor =
            [spStockModule, spCtx](const no::CPromise<COrderCtx>::ResolveFn& fnResolve,
                                   const no::CPromise<COrderCtx>::RejectFn& fnReject)
        {
            // 发起跨模块调用：执行器在库存模块内部，调用方不持有。
            auto spStock = std::make_shared<CCalleeCtx>();
            spStock->nSku = spCtx->nSku;
            spStock->spTrace = spCtx->spTrace;
            no::CPromise<CCalleeCtx> promiseStock = spStockModule->QueryStockAsync(spStock);

            const bool bOk = promiseStock.OnSettled([spCtx, spStock, fnResolve, fnReject](no::CPromiseResult result)
            {
                // 本回调在被调模块线程上执行：只做语义转换 + 改上下文 + settle。
                if (result.IsRejected())
                {
                    fnReject(result.Code());  // 库存模块拒绝 → 本流程拒绝（原样透传）。
                    return;
                }
                spCtx->nStock = spStock->nAvail;
                fnResolve();
            });
            if (!bOk)
            {
                // 子 promise 已 settled 且对方执行器不可用：回调不会执行，本层必须以拒绝收口
                // （否则本层永久 pending → 上层 Await 死等）。
                fnReject(no::kStopped);
            }
        };
        return no::CPromise<COrderCtx>::New(m_exec, spCtx, fnExecutor, ASYNC_LOC);
    }

    /// ⑤ 回到本模块线程：跨模块回调里显式投递到本模块执行器，再 settle 本层。
    ///
    /// @param spCtx 本流程上下文。
    ///
    /// @return 由本模块执行器上的任务 settle 的 promise。
    no::CPromise<COrderCtx> PostBackToOwnThread(const std::shared_ptr<COrderCtx>& spCtx)
    {
        no::CPromise<COrderCtx>::PromiseExecutor fnExecutor =
            [this](const no::CPromise<COrderCtx>::ResolveFn& fnResolve,
                   const no::CPromise<COrderCtx>::RejectFn& fnReject)
        {
            // executor 在本层所在线程（亲和后 = 本模块执行器线程）上同步执行，这里只投递、不干活。
            if (!m_exec.Post([fnResolve]()
            {
                fnResolve();
            }))
            {
                fnReject(no::kStopped);
            }
        };
        return no::CPromise<COrderCtx>::New(m_exec, spCtx, fnExecutor, ASYNC_LOC);
    }

    no::CAsyncExecutor m_exec;  ///< 模块私有执行器（单线程；析构自动 Stop）。
};

// ==================== 用例 ====================

/// @brief 跨模块调用的顺序与线程归属。
TEST(Module_OrderAndThreadOwnership)
{
    const std::thread::id idMain = std::this_thread::get_id();

    auto spStockModule = std::make_shared<CCalleeModule>();  // 1 线程
    auto spOrderModule = std::make_shared<COrderModule>();   // 1 线程

    auto spTrace = std::make_shared<CTraceSink>();

    auto spCtx = std::make_shared<COrderCtx>();
    spCtx->nSku = 7;
    spCtx->spTrace = spTrace;

    const no::CPromiseResult result = spOrderModule->PlaceOrderAsync(spCtx, spStockModule).Await();

    // 顺序：依赖边决定，跨模块也不乱
    ASSERT_TRUE(result.IsFulfilled());
    ASSERT_EQ(spTrace->strTrace, std::string("A1;B1;B2;A2;A3;"));
    ASSERT_EQ(spCtx->nStock, 5);

    // 线程：每个模块跑在自己的执行器线程上，且都不是调用线程
    ASSERT_TRUE(spCtx->idFirst != idMain);
    ASSERT_TRUE(spCtx->idAfterBridge != idMain);
    ASSERT_TRUE(spCtx->idBackHome != idMain);

    // 跨模块返回后那一层：线程亲和保证它恒在**本模块执行器线程**上
    ASSERT_TRUE(spCtx->idAfterBridge == spCtx->idFirst);
    ASSERT_TRUE(spCtx->idAfterBridge != spTrace->ThreadOf("B2"));

    // 显式投递回本模块执行器后，又回到本模块线程
    ASSERT_TRUE(spCtx->idBackHome == spCtx->idFirst);
}

/// @brief 单线程模块：并发调用也只是排队，模块内步骤不重叠。
TEST(Module_SingleThreadSerializesOwnSteps)
{
    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spTrace = std::make_shared<CTraceSink>();
    auto spProbe = std::make_shared<CStepProbe>();

    // 4 条并发查询：模块只有 1 个 worker，步骤不该重叠
    std::vector<std::shared_ptr<CCalleeCtx> > vecStock;
    std::vector<no::CPromise<CCalleeCtx> > vecPromise;
    for (int i = 0; i < 4; ++i)
    {
        auto spStock = std::make_shared<CCalleeCtx>();
        spStock->nSku = i;
        spStock->nDelayMs = 5;  // 拉长窗口：多线程时会真的重叠
        spStock->spTrace = spTrace;
        spStock->pProbe = spProbe;
        vecStock.push_back(spStock);
        vecPromise.push_back(spStockModule->QueryStockAsync(spStock));
    }

    for (size_t i = 0; i < vecPromise.size(); ++i)
    {
        ASSERT_TRUE(vecPromise[i].Await().IsFulfilled());
        ASSERT_EQ(vecStock[i]->nAvail, 5);
    }

    // 不重叠：任意时刻最多 1 个模块步骤在跑
    ASSERT_EQ(spProbe->nStockMaxInFlight.load(), 1);

    // 每条链自己的两步同线程、且 4 条链都在同一个模块线程上
    for (size_t i = 0; i < vecStock.size(); ++i)
    {
        ASSERT_TRUE(vecStock[i]->idConnect == vecStock[i]->idRead);
        ASSERT_TRUE(vecStock[i]->idConnect == vecStock[0]->idConnect);
    }

    // 顺序：单 worker + FIFO 投递 ⇒ 每条链的两步连在一起、链间依次排队
    ASSERT_EQ(spTrace->strTrace, std::string("B1;B2;B1;B2;B1;B2;B1;B2;"));
}

/// @brief 并发多条跨模块链：每条链自身顺序不乱，模块线程固定。
TEST(Module_ConcurrentChainsKeepOwnOrder)
{
    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<COrderModule>();

    const int nChains = 2;
    std::vector<std::shared_ptr<COrderCtx> > vecCtx;
    std::vector<no::CPromise<COrderCtx> > vecPromise;
    for (int i = 0; i < nChains; ++i)
    {
        auto spCtx = std::make_shared<COrderCtx>();
        spCtx->nSku = 100 + i;
        spCtx->spTrace = std::make_shared<CTraceSink>();  // 每条链自己的轨迹
        vecCtx.push_back(spCtx);
        vecPromise.push_back(spOrderModule->PlaceOrderAsync(spCtx, spStockModule));
    }

    for (int i = 0; i < nChains; ++i)
    {
        ASSERT_TRUE(vecPromise[i].Await().IsFulfilled());
    }

    for (int i = 0; i < nChains; ++i)
    {
        // 每条链自身顺序完整（并发下不交错、不缺步）
        ASSERT_EQ(vecCtx[i]->spTrace->strTrace, std::string("A1;B1;B2;A2;A3;"));
        ASSERT_EQ(vecCtx[i]->nStock, 5);

        // 模块线程固定：同一模块的自有步骤始终落在同一个（单）worker 上
        ASSERT_TRUE(vecCtx[i]->idFirst == vecCtx[0]->idFirst);
        ASSERT_TRUE(vecCtx[i]->idBackHome == vecCtx[i]->idFirst);

        // 跨模块续跑：线程亲和保证恒在本模块执行器线程
        ASSERT_TRUE(vecCtx[i]->idAfterBridge == vecCtx[i]->idFirst);
        ASSERT_TRUE(vecCtx[i]->idAfterBridge != vecCtx[i]->spTrace->ThreadOf("B2"));
    }
}
