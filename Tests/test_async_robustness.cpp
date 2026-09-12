/// @file test_async_robustness.cpp
/// 异步框架健壮性用例：用户代码「写法有问题 / 抛异常」时，框架既不终止进程、也不静默挂死。
///
/// 覆盖：
///  - 通知里抛异常：只报告 + 忽略，链结果不受影响（通知不是层）；
///  - `exec.Post` 投递的用户任务抛异常：兜住，worker 与后续任务照常；
///  - `AwaitFor(ms)`：超时不再永久挂住（返回 kStopped，不取消链）；
///  - 层内阻塞等待（`Await()`）：本身能返回但已属危险写法 → 框架给出死锁预警；
///  - 无效 promise 上挂层：报告误用（不再静默丢掉整条链）。

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "AsyncTestKit.h"
#include "TestFramework.h"

namespace no = common::async;

using asynctest::CCalleeCtx;
using asynctest::CCalleeModule;

// ==================== 测试助手 ====================

/// @brief 自旋等待条件成立（最多 2 秒），返回是否等到。
template <class TPred>
static bool WaitFor(TPred fnPred)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (fnPred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return fnPred();
}

/// @brief 捕获框架诊断的助手：构造时装上处理器，析构时恢复默认策略。
class CDiagnosticCapture
{
public:
    CDiagnosticCapture()
    {
        no::SetDiagnosticHandler(
            [this](const char* strWhat)
            {
                Append(strWhat);
            });
    }

    ~CDiagnosticCapture()
    {
        no::SetDiagnosticHandler(nullptr);  // 恢复默认（debug 打印 stderr，发布忽略）。
    }

    /// @brief 是否捕获到**完全等于**某文案的诊断（精确断言：文案改了就红，而不是默默通过）。
    bool Has(const char* strWhat) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t i = 0; i < m_vecMessages.size(); ++i)
        {
            if (m_vecMessages[i] == strWhat)
            {
                return true;
            }
        }
        return false;
    }

    /// @brief 是否捕获到包含某关键字的诊断。
    bool Contains(const char* strKeyword) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t i = 0; i < m_vecMessages.size(); ++i)
        {
            if (m_vecMessages[i].find(strKeyword) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    /// @brief 已捕获的诊断条数。
    int Count() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(m_vecMessages.size());
    }

private:
    void Append(const char* strWhat)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_vecMessages.push_back(strWhat != nullptr ? strWhat : "");
    }

    mutable std::mutex m_mutex;              ///< 保护 m_vecMessages（诊断可能来自工作线程）。
    std::vector<std::string> m_vecMessages;  ///< 捕获到的诊断描述。
};

/// @brief 最小上下文。
struct CRobustCtx
{
    int nValue;    ///< 简单数据。
    bool bCaught;  ///< 是否走到 catch 层。

    CRobustCtx() : nValue(0), bCaught(false)
    {}
};

/// @brief 首层：+1。
static no::CPromiseResult StepBump(no::CPromiseResult /*upResult*/, const std::shared_ptr<CRobustCtx>& spCtx)
{
    ++spCtx->nValue;
    return no::CPromiseResult::Resolve();
}

// ==================== 用例：用户回调异常不终止进程 ====================

/// @brief 通知里抛异常：链结果不受影响，进程存活，框架报告一次诊断。
TEST(Robust_NoticeThrowIsContained)
{
    CDiagnosticCapture capture;

    no::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    auto spCtx = std::make_shared<CRobustCtx>();
    no::CPromise<CRobustCtx> p = exec.NewPromise(spCtx, &StepBump, ASYNC_LOC);

    std::atomic<bool> bNoticeRan(false);
    ASSERT_TRUE(p.OnSettled(
        [&bNoticeRan](no::CPromiseResult)
        {
            bNoticeRan.store(true);
            throw std::runtime_error("通知里抛异常");  // 以前：逃出 settle → std::terminate。
        }));

    const no::CPromiseResult result = p.Await();
    ASSERT_TRUE(result.IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 1);

    ASSERT_TRUE(WaitFor(
        [&bNoticeRan]()
        {
            return bNoticeRan.load();
        }));
    // 诊断在抛异常之后才写入，故用 WaitFor 等它落地（避免读早于写）。
    ASSERT_TRUE(WaitFor(
        [&capture]()
        {
            return capture.Has(no::detail::kDiagNoticeThrow);
        }));
    exec.Stop();
}

/// @brief 已落定 + 执行器已停（通知走「就地送达」路径）时抛异常：同样只报告、不外抛。
TEST(Robust_NoticeThrowOnGuaranteedDeliveryPath)
{
    CDiagnosticCapture capture;

    auto spCtx = std::make_shared<CRobustCtx>();
    no::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());
    no::CPromise<CRobustCtx> p = exec.NewPromise(spCtx, &StepBump, ASYNC_LOC);
    ASSERT_TRUE(p.Await().IsFulfilled());
    exec.Stop();  // 执行器已停 → 通知只能就地送达。

    std::atomic<bool> bNoticeRan(false);
    ASSERT_TRUE(p.OnSettled(
        [&bNoticeRan](no::CPromiseResult)
        {
            bNoticeRan.store(true);
            throw std::runtime_error("就地送达的通知里抛异常");
        }));
    ASSERT_TRUE(WaitFor(
        [&bNoticeRan]()
        {
            return bNoticeRan.load();
        }));
    ASSERT_TRUE(WaitFor(
        [&capture]()
        {
            return capture.Has(no::detail::kDiagNoticeThrow);
        }));
}

/// @brief `exec.Post` 的用户任务抛异常：兜住；worker 与后续任务照常工作。
TEST(Robust_PostedTaskThrowIsContained)
{
    CDiagnosticCapture capture;

    no::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::atomic<bool> bSecondRan(false);
    ASSERT_TRUE(exec.Post(
        []()
        {
            throw std::runtime_error("投递的任务抛异常");
        }));
    ASSERT_TRUE(exec.Post(
        [&bSecondRan]()
        {
            bSecondRan.store(true);
        }));

    ASSERT_TRUE(WaitFor(
        [&bSecondRan]()
        {
            return bSecondRan.load();
        }));
    ASSERT_TRUE(capture.Has(no::detail::kDiagPostThrow));

    // 空任务：不提交 + 报告（不再返回 true 却什么也不做）。
    ASSERT_TRUE(!exec.Post(nullptr));
    ASSERT_TRUE(capture.Has(no::detail::kDiagPostEmpty));
    exec.Stop();
}

// ==================== 用例：AwaitFor 超时 / 死锁预警 ====================

/// @brief 永不落定的层：`AwaitFor(ms)` 按超时返回 kStopped，不再永久挂住。
TEST(Robust_AwaitForTimesOut)
{
    no::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    auto spCtx = std::make_shared<CRobustCtx>();
    no::CPromise<CRobustCtx> promisePending = no::CPromise<CRobustCtx>::New(
        exec, spCtx,
        [](const no::CPromise<CRobustCtx>::ResolveFn&, const no::CPromise<CRobustCtx>::RejectFn&)
        {
            // 故意不 settle：模拟「对端永远不回」。
        },
        ASYNC_LOC);

    const auto tBegin = std::chrono::steady_clock::now();
    const no::CPromiseResult result = promisePending.AwaitFor(50);
    const int nElapsedMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tBegin).count());

    ASSERT_TRUE(result.IsRejected());
    ASSERT_EQ(result.Code(), no::kStopped);    // 超时：只向调用方报「没等到」。
    ASSERT_TRUE(nElapsedMs >= 40);             // 确实等到了超时（不是立即返回）
    ASSERT_TRUE(nElapsedMs < 2000);            // 也没有挂死
    ASSERT_TRUE(!promisePending.IsSettled());  // 超时不落定本层（链仍在后台 pending）

    // 负超时 = 无限等待；已落定的层再 AwaitFor 立即拿结果。
    auto spDone = std::make_shared<CRobustCtx>();
    no::CPromise<CRobustCtx> promiseDone = exec.NewPromise(spDone, &StepBump, ASYNC_LOC);
    ASSERT_TRUE(promiseDone.AwaitFor(-1).IsFulfilled());
    ASSERT_TRUE(promiseDone.AwaitFor(0).IsFulfilled());
    exec.Stop();
}

/// @brief 层内阻塞等待由别的线程落定的层：能返回，但框架给出死锁预警（不硬失败）。
TEST(Robust_AwaitInsideLayerReportsRisk)
{
    CDiagnosticCapture capture;

    auto spCallee = std::make_shared<CCalleeModule>();
    auto spCalleeCtx = std::make_shared<CCalleeCtx>();
    spCalleeCtx->nDelayMs = 100;  // 保证「层开始等的时候子链还没落定」（在对方线程上跑）。
    no::CPromise<CCalleeCtx> promiseCallee = spCallee->QueryStockAsync(spCalleeCtx);

    no::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    auto spCtx = std::make_shared<CRobustCtx>();
    no::CPromise<CRobustCtx>::ThenHandler fnWaitInsideLayer =
        [promiseCallee](no::CPromiseResult /*upResult*/, const std::shared_ptr<CRobustCtx>& spCtx)
    {
        // 危险写法（但此处能返回：子链在对方模块线程上落定）：框架应给出预警。
        const no::CPromiseResult childResult = promiseCallee.Await();
        spCtx->bCaught = childResult.IsFulfilled();
        return no::CPromiseResult::Resolve();
    };

    const no::CPromiseResult result =
        exec.NewPromise(spCtx, &StepBump, ASYNC_LOC).Then(fnWaitInsideLayer, ASYNC_LOC).Await();

    ASSERT_TRUE(result.IsFulfilled());
    ASSERT_TRUE(spCtx->bCaught);
    ASSERT_TRUE(capture.Has(no::detail::kDiagAwaitRisk));  // 死锁预警已报告
    exec.Stop();
}

// ==================== 用例：无效 promise 上的误用 ====================

/// @brief 无效 promise 上挂层：空操作但**不静默**（报诊断），Await 仍是 kStopped。
TEST(Robust_InvalidPromiseAppendReports)
{
    CDiagnosticCapture capture;

    no::CPromise<CRobustCtx> promiseInvalid;  // 未绑定执行器
    ASSERT_TRUE(!promiseInvalid.IsValid());

    no::CPromise<CRobustCtx>::ThenHandler fnStep = [](no::CPromiseResult, const std::shared_ptr<CRobustCtx>&)
    {
        return no::CPromiseResult::Resolve();
    };
    auto fnCreateInvalid = [](const std::shared_ptr<CRobustCtx>&) -> no::CPromise<CCalleeCtx>
    {
        return no::CPromise<CCalleeCtx>();
    };
    auto fnApplyInvalid = [](const std::shared_ptr<CRobustCtx>&, const std::shared_ptr<CCalleeCtx>&)
    {
    };

    const no::CPromise<CRobustCtx> promiseAfterThen = promiseInvalid.Then(fnStep, ASYNC_LOC);
    const no::CPromise<CRobustCtx> promiseAfterBridge =
        promiseInvalid.ThenBridge(fnCreateInvalid, fnApplyInvalid, ASYNC_LOC);

    ASSERT_TRUE(!promiseAfterThen.IsValid());
    ASSERT_TRUE(!promiseAfterBridge.IsValid());
    ASSERT_EQ(promiseAfterThen.Await().Code(), no::kStopped);  // 既有语义不变
    ASSERT_TRUE(!promiseInvalid.OnSettled(
        [](no::CPromiseResult)
        {
        }));                                                  // 无效 promise 注册失败
    ASSERT_TRUE(capture.Has(no::detail::kDiagInvalidLayer));  // Then 路径
    ASSERT_TRUE(capture.Count() >= 2);                        // Then + ThenBridge 各一次
}

// ==================== 用例：OnSettledOn（通知落到指定执行器） ====================

/// @brief 通知跑在指定执行器线程上（而非结算线程），且执行器不可用时仍就地送达。
TEST(Robust_OnSettledOnRunsOnTargetExecutor)
{
    auto spCallee = std::make_shared<CCalleeModule>();
    auto spCalleeCtx = std::make_shared<CCalleeCtx>();
    no::CPromise<CCalleeCtx> promiseCallee = spCallee->QueryStockAsync(spCalleeCtx);  // 在对方模块线程上落定

    no::CAsyncExecutor ownExec(1);  // 「本模块」的执行器
    ASSERT_TRUE(ownExec.Start());

    // 先取本执行器的线程 id（用同样走 Post 的方式）。
    std::atomic<bool> bGotOwnThread(false);
    std::thread::id idOwnThread;
    ASSERT_TRUE(ownExec.Post(
        [&bGotOwnThread, &idOwnThread]()
        {
            idOwnThread = std::this_thread::get_id();
            bGotOwnThread.store(true);
        }));
    ASSERT_TRUE(WaitFor(
        [&bGotOwnThread]()
        {
            return bGotOwnThread.load();
        }));

    std::atomic<bool> bNoticeRan(false);
    std::thread::id idNoticeThread;
    ASSERT_TRUE(promiseCallee.OnSettledOn(ownExec,
        [&bNoticeRan, &idNoticeThread](no::CPromiseResult)
        {
            idNoticeThread = std::this_thread::get_id();
            bNoticeRan.store(true);
        }));
    ASSERT_TRUE(WaitFor(
        [&bNoticeRan]()
        {
            return bNoticeRan.load();
        }));
    ASSERT_TRUE(idNoticeThread == idOwnThread);              // 指定执行器线程
    ASSERT_TRUE(idOwnThread != std::this_thread::get_id());  // 不是调用线程
    ASSERT_TRUE(idNoticeThread != spCalleeCtx->idRead);      // 也不是被调模块线程
    ASSERT_TRUE(spCalleeCtx->nAvail == 5);                   // 父子链结果不受影响

    // 执行器已停：通知无法投递 → 就地送达（绝不丢）。
    ownExec.Stop();
    std::atomic<bool> bDeliveredAfterStop(false);
    ASSERT_TRUE(promiseCallee.OnSettledOn(ownExec,
        [&bDeliveredAfterStop](no::CPromiseResult)
        {
            bDeliveredAfterStop.store(true);
        }));
    ASSERT_TRUE(WaitFor(
        [&bDeliveredAfterStop]()
        {
            return bDeliveredAfterStop.load();
        }));
}
