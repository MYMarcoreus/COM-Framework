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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "AsyncTestKit.h"
#include "TestFramework.h"

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
    int nStockDelayMs;                    ///< 让库存模块每步耗时（覆盖「挂层早于 settle」路径）。
    std::shared_ptr<CTraceSink> spTrace;  ///< 轨迹（跨模块共享观测点）。
    std::thread::id idFirst;              ///< 本模块首层所在线程。
    std::thread::id idAfterBridge;        ///< 跨模块返回后那一层所在线程。
    std::thread::id idBackHome;           ///< 显式投递回本模块线程后那一层所在线程。

    COrderCtx() : nSku(0), nStock(0), nStockDelayMs(0)
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
    common::async::CPromise<COrderCtx> PlaceOrderAsync(
        const std::shared_ptr<COrderCtx>& spCtx, const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        // ③ 跨模块那一层：等库存模块的 promise（桥接层属于本模块）
        common::async::CPromise<COrderCtx>::PromiseFactory fnQueryStock = [this, spStockModule](
                                                                              const std::shared_ptr<COrderCtx>& spSelf)
        {
            return BridgeQueryStock(spSelf, spStockModule);
        };

        // ⑤ 回到本模块线程：显式投递到本模块执行器
        common::async::CPromise<COrderCtx>::PromiseFactory fnBackHome = [this](const std::shared_ptr<COrderCtx>& spSelf)
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

    /// @brief 下单流程（**ThenBridge 版**）：与 PlaceOrderAsync 逐项等价，只是跨模块那一层改用简写。
    ///
    /// 对比手写桥接（`BridgeQueryStock`）：省掉 `New` + `OnSettled` + resolve/reject 样板，
    /// 只留「怎么起子链」与「搬哪些数据回来」两件事。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spStockModule 库存模块。
    ///
    /// @return 指向最后一层的 promise。
    common::async::CPromise<COrderCtx> PlaceOrderByBridgeAsync(
        const std::shared_ptr<COrderCtx>& spCtx, const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        // ② 起子链：把入参搬进对方上下文（跑在本模块执行器线程上，只发起不干活）。
        auto fnCreateStock = [spStockModule](
                                 const std::shared_ptr<COrderCtx>& spSelf) -> common::async::CPromise<CCalleeCtx>
        {
            auto spStock = std::make_shared<CCalleeCtx>();
            spStock->nSku = spSelf->nSku;
            spStock->nDelayMs = spSelf->nStockDelayMs;
            spStock->spTrace = spSelf->spTrace;
            return spStockModule->QueryStockAsync(spStock);
        };

        // ③ 搬数据：子链兑现时把库存搬回本上下文（跑在被调模块线程上，只搬数据）。
        auto fnApplyStock = [](const std::shared_ptr<COrderCtx>& spSelf, const std::shared_ptr<CCalleeCtx>& spStock)
        {
            spSelf->nStock = spStock->nAvail;
        };

        // ⑤ 回到本模块线程：显式投递到本模块执行器（与手写版共用）。
        common::async::CPromise<COrderCtx>::PromiseFactory fnBackHome = [this](const std::shared_ptr<COrderCtx>& spSelf)
        {
            return PostBackToOwnThread(spSelf);
        };

        return m_exec
            .NewPromise(spCtx, &StepLoadOrder, ASYNC_LOC)        // ① 本模块执行器
            .ThenBridge(fnCreateStock, fnApplyStock, ASYNC_LOC)  // ② + ③ 库存模块（一行顶手写桥接）
            .Then(&StepTakeStock, ASYNC_LOC)                     // ④ 线程亲和：回本模块线程
            .ThenPromise(fnBackHome, ASYNC_LOC)                  // ⑤ 投递回本模块
            .Then(&StepFinish, ASYNC_LOC);                       // ⑥ 本模块执行器
    }

private:
    /// ① 读订单：本模块执行器线程。
    static common::async::CPromiseResult StepLoadOrder(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
    {
        spCtx->idFirst = std::this_thread::get_id();
        spCtx->spTrace->Append("A1");
        return common::async::CPromiseResult::Resolve();
    }

    /// ④ 跨模块返回后的层：记录本层线程（应为库存模块线程）。
    static common::async::CPromiseResult StepTakeStock(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
    {
        spCtx->idAfterBridge = std::this_thread::get_id();
        spCtx->spTrace->Append("A2");
        return common::async::CPromiseResult::Resolve();
    }

    /// ⑥ 回到本模块线程后的层：记录本层线程（应为本模块执行器线程）。
    static common::async::CPromiseResult StepFinish(
        common::async::CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
    {
        spCtx->idBackHome = std::this_thread::get_id();
        spCtx->spTrace->Append("A3");
        return common::async::CPromiseResult::Resolve();
    }

    /// ③ 桥接：把库存模块的 promise 接进本流程（等价 JS `new Promise`）。
    ///
    /// 桥接层用本模块执行器创建；库存模块在自己的执行器上跑。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spStockModule 库存模块。
    ///
    /// @return 由库存模块回调 settle 的本流程 promise。
    common::async::CPromise<COrderCtx> BridgeQueryStock(
        const std::shared_ptr<COrderCtx>& spCtx, const std::shared_ptr<CCalleeModule>& spStockModule)
    {
        common::async::CPromise<COrderCtx>::PromiseExecutor fnExecutor =
            [spStockModule, spCtx](const common::async::CPromise<COrderCtx>::ResolveFn& fnResolve,
                const common::async::CPromise<COrderCtx>::RejectFn& fnReject)
        {
            // 发起跨模块调用：执行器在库存模块内部，调用方不持有。
            auto spStock = std::make_shared<CCalleeCtx>();
            spStock->nSku = spCtx->nSku;
            spStock->nDelayMs = spCtx->nStockDelayMs;
            spStock->spTrace = spCtx->spTrace;
            common::async::CPromise<CCalleeCtx> promiseStock = spStockModule->QueryStockAsync(spStock);

            const bool bOk = promiseStock.OnSettled(
                [spCtx, spStock, fnResolve, fnReject](common::async::CPromiseResult result)
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
                fnReject(common::async::kStopped);
            }
        };
        return common::async::CPromise<COrderCtx>::New(m_exec, spCtx, fnExecutor, ASYNC_LOC);
    }

    /// ⑤ 回到本模块线程：跨模块回调里显式投递到本模块执行器，再 settle 本层。
    ///
    /// @param spCtx 本流程上下文。
    ///
    /// @return 由本模块执行器上的任务 settle 的 promise。
    common::async::CPromise<COrderCtx> PostBackToOwnThread(const std::shared_ptr<COrderCtx>& spCtx)
    {
        common::async::CPromise<COrderCtx>::PromiseExecutor fnExecutor =
            [this](const common::async::CPromise<COrderCtx>::ResolveFn& fnResolve,
                const common::async::CPromise<COrderCtx>::RejectFn& fnReject)
        {
            // executor 在本层所在线程（亲和后 = 本模块执行器线程）上同步执行，这里只投递、不干活。
            if (!m_exec.Post(
                    [fnResolve]()
                    {
                        fnResolve();
                    }))
            {
                fnReject(common::async::kStopped);
            }
        };
        return common::async::CPromise<COrderCtx>::New(m_exec, spCtx, fnExecutor, ASYNC_LOC);
    }

    common::async::CAsyncExecutor m_exec;  ///< 模块私有执行器（单线程；析构自动 Stop）。
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

    const common::async::CPromiseResult result = spOrderModule->PlaceOrderAsync(spCtx, spStockModule).Await();

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
    std::vector<common::async::CPromise<CCalleeCtx> > vecPromise;
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
    std::vector<common::async::CPromise<COrderCtx> > vecPromise;
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

// ==================== 用例：ThenBridge（跨模块桥接简写） ====================

/// @brief 测试用首层：记录本层线程 + 轨迹 "A1"（与订单模块的 ① 同形）。
static common::async::CPromiseResult StepLoadOrderForTest(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
{
    spCtx->idFirst = std::this_thread::get_id();
    spCtx->spTrace->Append("A1");
    return common::async::CPromiseResult::Resolve();
}

/// @brief `ThenBridge` 与手写桥接逐项等价：顺序、数据、线程归属、模块线程一致性。
TEST(Module_BridgeHelperEquivalentToManualBridge)
{
    auto spStockModule = std::make_shared<CCalleeModule>();
    auto spOrderModule = std::make_shared<COrderModule>();

    // 0ms：桥接层常常「登记晚于 settle」；5ms：拉长窗口走「登记早于 settle」那条路径。
    const int arrDelays[] = {0, 5};
    for (size_t i = 0; i < sizeof(arrDelays) / sizeof(arrDelays[0]); ++i)
    {
        auto spManualCtx = std::make_shared<COrderCtx>();
        spManualCtx->nSku = 7;
        spManualCtx->nStockDelayMs = arrDelays[i];
        spManualCtx->spTrace = std::make_shared<CTraceSink>();

        auto spBridgeCtx = std::make_shared<COrderCtx>();
        spBridgeCtx->nSku = 7;
        spBridgeCtx->nStockDelayMs = arrDelays[i];
        spBridgeCtx->spTrace = std::make_shared<CTraceSink>();

        ASSERT_TRUE(spOrderModule->PlaceOrderAsync(spManualCtx, spStockModule).Await().IsFulfilled());
        ASSERT_TRUE(spOrderModule->PlaceOrderByBridgeAsync(spBridgeCtx, spStockModule).Await().IsFulfilled());

        // 顺序与数据：两种写法一字不差
        ASSERT_EQ(spBridgeCtx->spTrace->strTrace, std::string("A1;B1;B2;A2;A3;"));
        ASSERT_EQ(spBridgeCtx->spTrace->strTrace, spManualCtx->spTrace->strTrace);
        ASSERT_EQ(spBridgeCtx->nStock, 5);
        ASSERT_EQ(spBridgeCtx->nStock, spManualCtx->nStock);

        // 线程归属：跨模块返回层回本模块线程、投递回本模块后仍在本模块线程
        ASSERT_TRUE(spBridgeCtx->idAfterBridge == spBridgeCtx->idFirst);
        ASSERT_TRUE(spBridgeCtx->idBackHome == spBridgeCtx->idFirst);
        ASSERT_TRUE(spBridgeCtx->idAfterBridge != spBridgeCtx->spTrace->ThreadOf("B2"));

        // 同模块的执行器线程固定：两种写法落在同一个（单）worker 上
        ASSERT_TRUE(spBridgeCtx->idFirst == spManualCtx->idFirst);
        ASSERT_TRUE(spBridgeCtx->idBackHome == spManualCtx->idBackHome);
        ASSERT_TRUE(spBridgeCtx->spTrace->ThreadOf("B2") == spManualCtx->spTrace->ThreadOf("B2"));
    }
}

/// @brief 子链被拒绝：本层以同一拒绝码被拒绝（后续 Then 跳过，Catch 仍可恢复）。
TEST(Module_BridgeHelperPropagatesRejection)
{
    auto spStockModule = std::make_shared<CCalleeModule>();
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    auto spCtx = std::make_shared<COrderCtx>();
    spCtx->spTrace = std::make_shared<CTraceSink>();

    auto fnCreateReject = [spStockModule](
                              const std::shared_ptr<COrderCtx>& spSelf) -> common::async::CPromise<CCalleeCtx>
    {
        auto spStock = std::make_shared<CCalleeCtx>();
        spStock->bReject = true;  // 让库存模块在第二步拒绝（码 = kRejectCode = kBusinessBase）。
        spStock->spTrace = spSelf->spTrace;
        return spStockModule->QueryStockAsync(spStock);
    };
    auto fnApplyNever = [](const std::shared_ptr<COrderCtx>& spSelf, const std::shared_ptr<CCalleeCtx>& spStock)
    {
        spSelf->nStock = spStock->nAvail;  // 子链被拒绝时不会被调用（nAvail 不应被搬走）。
    };
    auto fnAfterBridge = [](common::async::CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
    {
        spCtx->idAfterBridge = std::this_thread::get_id();
        spCtx->spTrace->Append("A2");
        return common::async::CPromiseResult::Resolve();
    };
    int nCaughtCode = 0;
    auto fnCatch = [&nCaughtCode](common::async::CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
    {
        nCaughtCode = upResult.Code();  // 桥接层把子链的拒绝码原样透传到这里。
        spCtx->spTrace->Append("C1");
        return common::async::CPromiseResult::Resolve();  // 吞掉拒绝：链从此处继续。
    };
    auto fnAfterCatch = [](common::async::CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
    {
        spCtx->spTrace->Append("A3");
        return common::async::CPromiseResult::Resolve();
    };

    // 桥接那一层的结果：以子链的拒绝码被拒绝（不是 kRejected、也不是 kStopped）。
    common::async::CPromise<COrderCtx> promiseBridge =
        exec.NewPromise(spCtx, &StepLoadOrderForTest, ASYNC_LOC).ThenBridge(fnCreateReject, fnApplyNever, ASYNC_LOC);

    // 后续层：A2 跳过 → Catch 看到同一拒绝码并恢复 → A3 执行。
    const common::async::CPromiseResult resultTail =
        promiseBridge.Then(fnAfterBridge, ASYNC_LOC).Catch(fnCatch, ASYNC_LOC).Then(fnAfterCatch, ASYNC_LOC).Await();

    const common::async::CPromiseResult resultBridge = promiseBridge.Await();
    ASSERT_TRUE(resultBridge.IsRejected());
    ASSERT_EQ(resultBridge.Code(), common::async::kBusinessBase);  // 拒绝码原样透传（不是 kRejected）。
    ASSERT_TRUE(resultTail.IsFulfilled());                         // Catch 已恢复：链尾兑现
    ASSERT_EQ(nCaughtCode, common::async::kBusinessBase);
    ASSERT_EQ(spCtx->nStock, 0);                                          // 被拒绝 → 不搬数据
    ASSERT_EQ(spCtx->spTrace->strTrace, std::string("A1;B1;B2;C1;A3;"));  // A2 跳过，Catch 恢复后 A3 执行
}

/// @brief 子链无效（工厂返回无效 promise）：本层以 kStopped 拒绝，不挂死。
TEST(Module_BridgeHelperInvalidChildRejects)
{
    auto spCtx = std::make_shared<COrderCtx>();
    spCtx->spTrace = std::make_shared<CTraceSink>();

    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    auto fnCreateInvalid = [](const std::shared_ptr<COrderCtx>&) -> common::async::CPromise<CCalleeCtx>
    {
        return common::async::CPromise<CCalleeCtx>();  // 无效 promise：没有可等待的子链。
    };
    auto fnApplyNever = [](const std::shared_ptr<COrderCtx>&, const std::shared_ptr<CCalleeCtx>&)
    {
    };

    const common::async::CPromiseResult result = exec.NewPromise(spCtx, &StepLoadOrderForTest, ASYNC_LOC)
                                                     .ThenBridge(fnCreateInvalid, fnApplyNever, ASYNC_LOC)
                                                     .Await();

    ASSERT_TRUE(result.IsRejected());
    ASSERT_EQ(result.Code(), common::async::kStopped);
    ASSERT_EQ(spCtx->spTrace->strTrace, std::string("A1;"));
}

/// @brief 搬数据时抛异常：本层以 kException 拒绝（不让异常窜出通知回调）。
TEST(Module_BridgeHelperApplyThrowRejects)
{
    auto spStockModule = std::make_shared<CCalleeModule>();
    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    auto spCtx = std::make_shared<COrderCtx>();
    spCtx->spTrace = std::make_shared<CTraceSink>();

    auto fnCreateStock = [spStockModule](
                             const std::shared_ptr<COrderCtx>& spSelf) -> common::async::CPromise<CCalleeCtx>
    {
        auto spStock = std::make_shared<CCalleeCtx>();
        spStock->spTrace = spSelf->spTrace;
        return spStockModule->QueryStockAsync(spStock);
    };
    auto fnApplyThrow = [](const std::shared_ptr<COrderCtx>&, const std::shared_ptr<CCalleeCtx>&)
    {
        throw std::runtime_error("搬数据失败");  // 处理器里抛异常 → 本层 kException。
    };
    int nCaughtCode = 0;
    auto fnCatch = [&nCaughtCode](common::async::CPromiseResult upResult, const std::shared_ptr<COrderCtx>&)
    {
        nCaughtCode = upResult.Code();
        return common::async::CPromiseResult::Resolve();
    };

    const common::async::CPromiseResult result = exec.NewPromise(spCtx, &StepLoadOrderForTest, ASYNC_LOC)
                                                     .ThenBridge(fnCreateStock, fnApplyThrow, ASYNC_LOC)
                                                     .Catch(fnCatch, ASYNC_LOC)
                                                     .Await();

    ASSERT_TRUE(result.IsFulfilled());  // Catch 恢复了结果
    ASSERT_EQ(nCaughtCode, common::async::kException);
    ASSERT_EQ(spCtx->nStock, 0);
    ASSERT_EQ(spCtx->spTrace->strTrace, std::string("A1;B1;B2;"));  // 子链照常跑完
}
