/// @file test_async_settled_delivery.cpp
/// 改进：`OnSettled` 送达保证（问题 ② 的框架层修复）——调用方不再需要检查返回值。
///
/// 背景：`OnSettled` 以前在「本层已 settled 且它的执行器不可用」（被调模块已停止 / 拒绝投递）
/// 时返回 `false` 且**丢弃回调**。手写桥接若不检查返回值，桥接层就永久 pending，
/// 上层 `Await()` 死等（实测：`ModuleStress_StopMidFlight` 修复前 45s 超时挂住）。
///
/// 现在：`OnSettled` **保证送达** —— 执行器可用时投递（不阻塞调用方），不可用时在调用线程上
/// 就地执行；返回值只在「promise 无效」时才为 `false`。
///
/// 边界（本文件同时验收）：层处理器（then / catch / finally）**不变**——执行器不可用时
/// 仍以 `kStopped` 收口，「停了的执行器不再跑新层」。

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "TestFramework.h"

namespace no = common::async;

// ==================== 可停止的被调模块（1 线程） ====================

/// @brief 被调模块上下文。
struct CDeliveryCtx
{
    std::atomic<int> nStepRuns;  ///< 本模块层处理器执行次数。
    int nMarkRuns;               ///< 测试追加层的执行次数。
    std::string strNotifyOrder;  ///< 通知送达顺序（就地送达时单线程写入，无需加锁）。

    CDeliveryCtx() : nStepRuns(0), nMarkRuns(0)
    {}
};

/// @brief 被调模块：自持 1 线程执行器，可显式 Stop。
class CDeliveryModule
{
public:
    CDeliveryModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 停止本模块执行器（之后的投递一律被拒绝）。
    void Stop()
    {
        m_exec.Stop();
    }

    /// @brief 查询（一层）：执行器不可用时首层以 `kStopped` 被拒绝。
    ///
    /// @param spCtx 本模块上下文。
    ///
    /// @return 本层 promise。
    no::CPromise<CDeliveryCtx> QueryAsync(const std::shared_ptr<CDeliveryCtx>& spCtx)
    {
        return m_exec.NewPromise(spCtx, &StepQuery, ASYNC_LOC);
    }

private:
    /// 层处理器：记录执行次数。
    static no::CPromiseResult StepQuery(no::CPromiseResult /*upResult*/, const std::shared_ptr<CDeliveryCtx>& spCtx)
    {
        ++spCtx->nStepRuns;
        return no::CPromiseResult::Resolve();
    }

    no::CAsyncExecutor m_exec;  ///< 模块私有执行器（单线程）。
};

/// @brief 测试用追加层（记录执行次数）。
static no::CPromiseResult StepMark(no::CPromiseResult /*upResult*/, const std::shared_ptr<CDeliveryCtx>& spCtx)
{
    ++spCtx->nMarkRuns;
    return no::CPromiseResult::Resolve();
}

// ==================== 调用方模块（桥接里故意不检查返回值） ====================

/// @brief 调用方上下文。
struct CCallerCtx
{
    int nOwnSteps;    ///< 本模块自有层执行次数。
    bool bCaught;     ///< catch 是否执行。
    int nCaughtCode;  ///< catch 收到的码。
    int nNotify;      ///< 桥接里收到的通知次数。

    CCallerCtx() : nOwnSteps(0), bCaught(false), nCaughtCode(0), nNotify(0)
    {}
};

/// @brief 调用方模块：桥接里**故意不检查** `OnSettled` 返回值（模拟用户代码）。
class CCallerModule
{
public:
    CCallerModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 跑一条跨模块链：A1 → 等被调模块 → A2 → catch 兜底。
    ///
    /// @param spCtx 本流程上下文。
    /// @param spCallee 被调模块（可为已停止的模块）。
    ///
    /// @return 指向最后一层的 promise。
    no::CPromise<CCallerCtx> RunAsync(const std::shared_ptr<CCallerCtx>& spCtx,
                                      const std::shared_ptr<CDeliveryModule>& spCallee)
    {
        no::CPromise<CCallerCtx>::PromiseFactory fnCall = [this, spCallee](const std::shared_ptr<CCallerCtx>& spSelf)
        {
            return BridgeCallCallee(spSelf, spCallee);
        };

        return m_exec.NewPromise(spCtx, &StepOrderA, ASYNC_LOC)
            .ThenPromise(fnCall, ASYNC_LOC)
            .Then(&StepOrderB, ASYNC_LOC)
            .Catch(&StepCatch, ASYNC_LOC);
    }

private:
    /// ① 本模块自有层。
    static no::CPromiseResult StepOrderA(no::CPromiseResult /*upResult*/, const std::shared_ptr<CCallerCtx>& spCtx)
    {
        ++spCtx->nOwnSteps;
        return no::CPromiseResult::Resolve();
    }

    /// ③ 跨模块返回后的层（只有被调模块兑现才会执行）。
    static no::CPromiseResult StepOrderB(no::CPromiseResult /*upResult*/, const std::shared_ptr<CCallerCtx>& spCtx)
    {
        ++spCtx->nOwnSteps;
        return no::CPromiseResult::Resolve();
    }

    /// 兜底：记录拒绝码。
    static no::CPromiseResult StepCatch(no::CPromiseResult upResult, const std::shared_ptr<CCallerCtx>& spCtx)
    {
        spCtx->bCaught = true;
        spCtx->nCaughtCode = upResult.Code();
        return upResult;
    }

    /// ② 桥接层：**故意不检查** `promiseCallee.OnSettled(...)` 的返回值。
    ///
    /// 框架已保证送达（执行器不可用时就地执行），因此这里漏检也不会永久 pending。
    no::CPromise<CCallerCtx> BridgeCallCallee(const std::shared_ptr<CCallerCtx>& spCtx,
                                              const std::shared_ptr<CDeliveryModule>& spCallee)
    {
        no::CPromise<CCallerCtx>::PromiseExecutor fnExecutor =
            [spCallee, spCtx](const no::CPromise<CCallerCtx>::ResolveFn& fnResolve,
                              const no::CPromise<CCallerCtx>::RejectFn& fnReject)
        {
            auto spCalleeCtx = std::make_shared<CDeliveryCtx>();
            no::CPromise<CDeliveryCtx> promiseCallee = spCallee->QueryAsync(spCalleeCtx);

            // 注意：返回值被丢弃（模拟漏检的调用方代码）
            promiseCallee.OnSettled(
                [spCtx, fnResolve, fnReject](no::CPromiseResult result)
                {
                    ++spCtx->nNotify;
                    if (result.IsRejected())
                    {
                        fnReject(result.Code());
                        return;
                    }
                    fnResolve();
                });
        };
        return no::CPromise<CCallerCtx>::New(m_exec, spCtx, fnExecutor, ASYNC_LOC);
    }

    no::CAsyncExecutor m_exec;  ///< 模块私有执行器（单线程）。
};

// ==================== 用例 ====================

/// @brief 执行器已停止时，已 settled 的 promise 上注册的通知仍被送达（就地执行）。
TEST(SettledNotice_DeliveredEvenIfExecutorStopped)
{
    const std::thread::id idCaller = std::this_thread::get_id();

    auto spModule = std::make_shared<CDeliveryModule>();
    auto spCtx = std::make_shared<CDeliveryCtx>();

    spModule->Stop();  // 先停：后续投递一律被拒绝
    no::CPromise<CDeliveryCtx> promise = spModule->QueryAsync(spCtx);

    const no::CPromiseResult result = promise.Await();
    ASSERT_TRUE(result.IsRejected());
    ASSERT_EQ(result.Code(), no::kStopped);
    ASSERT_EQ(spCtx->nStepRuns.load(), 0);  // 首层没跑（投递失败）

    // 在「已 settled + 执行器不可用」的层上注册通知：必须被送达
    int nNotify = 0;
    std::thread::id idNotified;
    const bool bOk = promise.OnSettled(
        [&nNotify, &idNotified](no::CPromiseResult r)
        {
            ++nNotify;
            idNotified = std::this_thread::get_id();
            ASSERT_TRUE(r.IsRejected());  // 通知里能看到真实结果
        });

    ASSERT_TRUE(bOk);                       // 不再返回 false（唯一 false 是无效 promise）
    ASSERT_EQ(nNotify, 1);                  // 回调确实执行了
    ASSERT_TRUE(idNotified == idCaller);    // 就地送达：在调用线程上执行
    ASSERT_EQ(spCtx->nStepRuns.load(), 0);  // 仍然没有跑"层"（层不被就地执行）
}

/// @brief 同一次注册多个通知：全部送达、按注册顺序、都在调用线程上。
TEST(SettledNotice_ManyRegistrationsAllDelivered)
{
    const std::thread::id idCaller = std::this_thread::get_id();
    const int nCount = 100;

    auto spModule = std::make_shared<CDeliveryModule>();
    auto spCtx = std::make_shared<CDeliveryCtx>();

    spModule->Stop();
    no::CPromise<CDeliveryCtx> promise = spModule->QueryAsync(spCtx);
    ASSERT_TRUE(promise.Await().IsRejected());

    int nNotify = 0;
    std::string strOrder;
    std::thread::id idNotified;
    for (int i = 0; i < nCount; ++i)
    {
        std::string strSeq = std::to_string(i);
        strSeq += ';';
        const bool bOk = promise.OnSettled(
            [&nNotify, &strOrder, &idNotified, strSeq](no::CPromiseResult /*r*/)
            {
                ++nNotify;
                strOrder += strSeq;
                idNotified = std::this_thread::get_id();
            });
        ASSERT_TRUE(bOk);
    }

    ASSERT_EQ(nNotify, nCount);
    ASSERT_TRUE(idNotified == idCaller);
    std::string strExpected;
    for (int i = 0; i < nCount; ++i)
    {
        strExpected += std::to_string(i);
        strExpected += ';';
    }
    ASSERT_EQ(strOrder, strExpected);  // 注册顺序 = 送达顺序
}

/// @brief 唯一仍返回 false 的情形：无效 promise（未绑定执行器）。
TEST(SettledNotice_InvalidPromiseReturnsFalse)
{
    int nNotify = 0;
    no::CPromise<CDeliveryCtx> promiseInvalid;  // 默认构造：无效
    const bool bOk = promiseInvalid.OnSettled(
        [&nNotify](no::CPromiseResult /*r*/)
        {
            ++nNotify;
        });

    ASSERT_TRUE(bOk == false);
    ASSERT_EQ(nNotify, 0);
    ASSERT_TRUE(promiseInvalid.Await().IsRejected());  // 无效 promise：Await 返回 kStopped
}

/// @brief 关键回归：桥接里**漏检**返回值（问题 ② 的原形状）→ 链以 kStopped 拒绝，不死等。
TEST(SettledNotice_BridgeWithoutReturnCheckNoDeadlock)
{
    auto spCallee = std::make_shared<CDeliveryModule>();
    auto spCaller = std::make_shared<CCallerModule>();
    auto spCtx = std::make_shared<CCallerCtx>();

    spCallee->Stop();  // 被调模块先停：桥接里的 OnSettled 落在"已 settled + 执行器不可用"这条路径上

    // 修复前：这里会永久阻塞（桥接层永久 pending）
    const no::CPromiseResult result = spCaller->RunAsync(spCtx, spCallee).Await();

    ASSERT_TRUE(result.IsRejected());
    ASSERT_EQ(result.Code(), no::kStopped);  // 被调模块的拒绝码原样透传
    ASSERT_EQ(spCtx->nNotify, 1);            // 桥接通知被送达
    ASSERT_TRUE(spCtx->bCaught);             // 兜底执行
    ASSERT_EQ(spCtx->nCaughtCode, no::kStopped);
    ASSERT_EQ(spCtx->nOwnSteps, 1);  // 只有 StepOrderA 执行（跨模块之后的层被跳过）
}

/// @brief 对照：层处理器（then）不变 —— 执行器不可用时仍以 kStopped 收口，不就地执行。
TEST(SettledNotice_LayerStillRejectedWhenExecutorUnavailable)
{
    auto spModule = std::make_shared<CDeliveryModule>();
    auto spCtx = std::make_shared<CDeliveryCtx>();

    no::CPromise<CDeliveryCtx> promise = spModule->QueryAsync(spCtx);
    ASSERT_TRUE(promise.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nStepRuns.load(), 1);

    spModule->Stop();  // 之后执行器不可用

    // 已 settled + 执行器不可用：追加层 → 本层以 kStopped 结算（不是就地执行）
    const no::CPromiseResult result = promise.Then(&StepMark, ASYNC_LOC).Await();

    ASSERT_TRUE(result.IsRejected());
    ASSERT_EQ(result.Code(), no::kStopped);
    ASSERT_EQ(spCtx->nMarkRuns, 0);  // "停了的执行器不再跑新层"
}
