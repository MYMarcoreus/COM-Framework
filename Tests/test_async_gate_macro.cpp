/// @file test_async_gate_macro.cpp
/// 层体「自请过门」（`ASYNC_GATE`）的专项测试。
///
/// 正确性验证点：
///  - **调用点省略类别**（缺省 `kDirect` = 不过门）：本层没有槽位、与写者并发（不被门挡）；
///  - 函数体声明与现状一致 → **零成本落穿**（就地执行：与外层是同一个任务帧，不产生新任务）；
///  - 声明与现状不同 → **挂起 → 按声明类别过门重入**：
///      · 直投 / 读 → 写（升级）：真等读者（或写者）让位，重入后帧 = 写；
///      · 直投 → 读（换档）：与其它读并发（读段峰值 ≥ 2），重入后帧 = 读；
///      · `kDirect`：直接落穿（不过门）；
///  - 挂起**不改变链语义**：本层未 settle → 下游不提前跑；函数体只跑一次；
///    兑现 / 拒绝（自定义异常类型 + 文案）原样透传；catch 恢复、finally 不改结果、首层同样可用；
///  - **同一函数体里 `ASYNC_GATE` 只允许出现一次**：宏里带固定标签，第二处 = `duplicate label`
///    **编译错误**（由 `.tools/check_async_gate_once.sh` 守着）；漏网的换档写法（辅助 lambda 里再声明门 /
///    绕过宏直接请求 `EnterGate`）由运行时兜底收口 —— **一次运行只允许挂起一次**，第二次挂起即有界失败；
///  - 并发正确性：声明写门的层与模块自己的写任务**零重叠**、计数精确；声明读门可并发；
///  - 误用（没有层体作用域）的**诊断**在 `test_async_robustness.cpp::Robust_GateGuardOutsideLayerDiagnosed`
///    里（那里有诊断捕获助手），本文件只测过门语义，不重复。
///
/// 说明：断言只允许在主测试线程执行；工作线程仅更新原子状态。
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/GateGuard.h"
#include "Async/Promise.h"
#include "AsyncTestKit.h"
#include "TestFramework.h"

namespace {

using common::async::CAsyncExecutor;
using common::async::CPromise;
using common::async::CPromiseResult;
using common::async::TaskKind;
using common::async::detail::EGateEnter;
using common::async::detail::EnterGate;

/// @brief 业务异常（验证「挂起重入」之后异常类型与文案仍然保真）。
class CGateMacroError : public std::runtime_error
{
public:
    /// @brief 构造。
    ///
    /// @param strWhat 描述文案。
    explicit CGateMacroError(const std::string& strWhat) : std::runtime_error(strWhat)
    {}
};

/// @brief 专项测试的共享上下文（每条链一个；并发用例里多条链共用）。
struct SGateCtx
{
    SGateCtx() : nRuns(0), nKind(-2), pFrame(NULL), nActive(0), nOverlaps(0), nStateCount(0), nActiveReads(0), nPeakReaders(0)
    {}

    std::atomic<int> nRuns;  ///< 处理器跑了几次（应为 1：挂起重入不算两次）。
    std::atomic<int> nKind;  ///< 帧上的类别（-1 = 没有帧 / 不过门）。
    std::atomic<const common::async::detail::CTaskFrame*> pFrame;  ///< 首层记下的任务帧（验证就地落穿）。
    std::atomic<int> nActive;                                      ///< 写状态段内的进入者数（观测用）。
    std::atomic<int> nOverlaps;                                    ///< 写状态段的重叠进入次数（应为 0）。
    std::atomic<int> nStateCount;                                  ///< 写状态段里累计做的「状态修改」次数。
    std::atomic<int> nActiveReads;                                 ///< 读段内的并发数（观测读是否真并发）。
    std::atomic<int> nPeakReaders;                                 ///< 读段并发峰值（应 ≥ 2）。
};

/// @brief 记一笔「函数体跑了 + 以什么类别跑的」。
///
/// @param spCtx 共享上下文。
void RecordRun(const std::shared_ptr<SGateCtx>& spCtx)
{
    spCtx->nRuns.fetch_add(1);
    const common::async::detail::CTaskFrame* pFrame = common::async::detail::CTaskFrame::Top();
    spCtx->nKind.store(pFrame != NULL ? static_cast<int>(pFrame->eKind) : -1);
}

/// @brief 进入「写状态段」：已有别的进入者 = 记一次重叠；停留一小段便于观测。
///
/// @param spCtx 共享上下文。
void EnterWriteSection(const std::shared_ptr<SGateCtx>& spCtx)
{
    if (spCtx->nActive.fetch_add(1) != 0)
    {
        spCtx->nOverlaps.fetch_add(1);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}

/// @brief 离开「写状态段」。
///
/// @param spCtx 共享上下文。
void LeaveWriteSection(const std::shared_ptr<SGateCtx>& spCtx)
{
    spCtx->nActive.fetch_sub(1);
}

/// @brief 进入「读段」：记录并发峰值；停留一小段便于观测。
///
/// @param spCtx 共享上下文。
void EnterReadSection(const std::shared_ptr<SGateCtx>& spCtx)
{
    const int nNow = spCtx->nActiveReads.fetch_add(1) + 1;
    int nPeak = spCtx->nPeakReaders.load();
    while (nNow > nPeak && !spCtx->nPeakReaders.compare_exchange_weak(nPeak, nNow))
    {
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}

/// @brief 离开「读段」。
///
/// @param spCtx 共享上下文。
void LeaveReadSection(const std::shared_ptr<SGateCtx>& spCtx)
{
    spCtx->nActiveReads.fetch_sub(1);
}

// ====================================================================
// 具名处理器（本文档的「用法样例」：函数自己声明门要求）
// ====================================================================

/// @brief 首层：只记下当前任务帧（用于验证后续层是否**就地**落穿在同一帧里）。
///
/// @param spCtx 共享上下文。
/// @return 兑现。
CPromiseResult StepCaptureFrame(const std::shared_ptr<SGateCtx>& spCtx)
{
    spCtx->pFrame.store(common::async::detail::CTaskFrame::Top());
    return CPromiseResult::Resolve();
}

/// @brief 声明写门，并记「是否仍在首层那个帧里」（同类 → 就地落穿，不产生新任务）。
///
/// @param spCtx 共享上下文。
/// @return 兑现。
CPromiseResult StepSameFrameGated(const std::shared_ptr<SGateCtx>& spCtx)
{
    ASYNC_GATE_WRITE();  // ← 必须第一条语句
    const bool bSameFrame = (common::async::detail::CTaskFrame::Top() == spCtx->pFrame.load());
    spCtx->nKind.store(bSameFrame ? static_cast<int>(TaskKind::kWrite) : -3);
    spCtx->nRuns.fetch_add(1);
    return CPromiseResult::Resolve();
}

/// @brief 声明写门 + 走一段「改状态」的临界区（并发用例用它）。
///
/// @param spCtx 共享上下文。
/// @return 兑现。
CPromiseResult StepWriteGated(const std::shared_ptr<SGateCtx>& spCtx)
{
    ASYNC_GATE_WRITE();  // ← 必须第一条语句
    RecordRun(spCtx);
    EnterWriteSection(spCtx);
    spCtx->nStateCount.fetch_add(1);
    LeaveWriteSection(spCtx);
    return CPromiseResult::Resolve();
}

/// @brief 声明读门 + 记读段并发峰值（读声明应当并发，不该被串行化）。
///
/// @param spCtx 共享上下文。
/// @return 兑现。
CPromiseResult StepReadGated(const std::shared_ptr<SGateCtx>& spCtx)
{
    ASYNC_GATE_READ();  // ← 必须第一条语句
    RecordRun(spCtx);
    EnterReadSection(spCtx);
    LeaveReadSection(spCtx);
    return CPromiseResult::Resolve();
}

/// @brief 声明写门，然后**返回业务拒绝**（验证类型 + 文案在挂起重入后仍然保真）。
///
/// @param spCtx 共享上下文。
/// @return 业务拒绝（`CGateMacroError`）。
CPromiseResult StepWriteGatedReject(const std::shared_ptr<SGateCtx>& spCtx)
{
    ASYNC_GATE_WRITE();  // ← 必须第一条语句
    RecordRun(spCtx);
    return CPromiseResult::Reject(CGateMacroError("声明写门后拒绝"));
}

/// @brief 同一类别的**重复请求**（函数形态）：第二处落穿（已在槽位 → 继续），只挂起一次。
///
/// 说明：这里刻意不用宏 —— 一个函数里写两次 `ASYNC_GATE` 是**编译错误**（`duplicate label`）；
/// 只有函数形态才能表达「绕过编译期检查的重复请求」。
///
/// @param spCtx 共享上下文。
/// @return 兑现。
CPromiseResult StepSameKindTwiceRequests(const std::shared_ptr<SGateCtx>& spCtx)
{
    if (EGateEnter::kReenter == EnterGate(TaskKind::kRead))  // ← 第一条请求：挂起 → 过门重入
    {
        return CPromiseResult::Resolve();
    }
    if (EGateEnter::kReenter == EnterGate(TaskKind::kRead))  // ← 第二条请求：已在读槽位 → 落穿
    {
        return CPromiseResult::Resolve();
    }
    RecordRun(spCtx);
    return CPromiseResult::Resolve();
}

/// @brief **绕过宏**先后请求两种类别（读 → 写）：运行时兜底把它收成有界失败（第二次挂起即收口）。
///
/// 说明：宏形态的两种类别是**编译错误**（`duplicate label`，见 `Common/Async/GateGuard.h`），只有
/// 「跨作用域各声明一次」或「直接调 `EnterGate`」能绕开编译期检查，本条挂的处理器守的就是这条兜底路径。
///
/// @param spCtx 共享上下文。
/// @return 兑现（正常情况下走不到这里：本层会被「重复挂起」收口）。
CPromiseResult StepMixedKindRequests(const std::shared_ptr<SGateCtx>& spCtx)
{
    if (EGateEnter::kReenter == EnterGate(TaskKind::kRead))  // ← 要读：挂起 → 按读过门重入
    {
        return CPromiseResult::Resolve();
    }
    if (EGateEnter::kReenter == EnterGate(TaskKind::kWrite))  // ← 又要写：与上一条来回换档 → 上限收口
    {
        return CPromiseResult::Resolve();
    }
    RecordRun(spCtx);
    return CPromiseResult::Resolve();
}

/// @brief 层体里调用**带宏的辅助 lambda**（编译期管不到的漏网写法）：外层要写、内层要读 → 换档。
///
/// 辅助 lambda 有自己的函数作用域（标签不冲突，编译器放行），但它与层体**共享同一份挂起作用域**：
/// 外层挂起 → 重入（帧 = 写）→ 外层落穿 → 内层请求读 → 又一次挂起 → 换档 …… 直到兜底收口。
/// 正确做法：把那个 lambda 直接挂成一层（`.Then(该 lambda)`），而不是在层体里内联调用它。
///
/// @param spCtx 共享上下文。
/// @return 兑现（正常情况下走不到这里：本层会被「重复挂起」收口）。
CPromiseResult StepOuterWithGatedHelper(const std::shared_ptr<SGateCtx>& spCtx)
{
    ASYNC_GATE_WRITE();  // 外层：要写

    const auto fnHelper = [&spCtx]() -> CPromiseResult
    {
        ASYNC_GATE_READ();  // 内层：要读（换档点；辅助函数里再声明门 = 误用）
        RecordRun(spCtx);
        return CPromiseResult::Resolve();
    };
    fnHelper();
    return CPromiseResult::Resolve();
}

/// @brief 请求 `kDirect`（不过门）：应当直接落穿。
///
/// @param spCtx 共享上下文。
/// @return 兑现。
CPromiseResult StepDirectRequest(const std::shared_ptr<SGateCtx>& spCtx)
{
    ASYNC_GATE(kDirect);  // 直投：不需要门，直接继续
    RecordRun(spCtx);
    return CPromiseResult::Resolve();
}

/// @brief catch 层：声明写门后**吞掉拒绝**（返回 `Resolve()` 恢复链）。
///
/// @param upResult 上一层结果（catch 里必定是拒绝）。
/// @param spCtx 共享上下文。
/// @return 兑现（恢复链）。
CPromiseResult StepCatchGated(CPromiseResult upResult, const std::shared_ptr<SGateCtx>& spCtx)
{
    ASYNC_GATE_WRITE();  // ← 必须第一条语句
    (void)upResult;      // 本例不消费旧结果（是否消费不影响验证点）。
    RecordRun(spCtx);
    return CPromiseResult::Resolve();
}

/// @brief finally 层：声明写门；finally「不改结果」，重入也不该改变这一点。
///
/// @param upResult 上一层结果。
/// @param spCtx 共享上下文。
/// @return `upResult`（原样透传）。
CPromiseResult StepFinallyGated(CPromiseResult upResult, const std::shared_ptr<SGateCtx>& spCtx)
{
    ASYNC_GATE_WRITE();  // ← 必须第一条语句
    RecordRun(spCtx);
    return upResult;
}

}  // namespace

// ====================================================================
// 一、落穿 / 不过门（常态路径）
// ====================================================================

/// @brief 同类声明 → **就地落穿**：与外层同一个任务帧（不产生新任务）、函数体只跑一次。
///
/// 确定性做法：先用一个「占住唯一 worker」的写任务锁住执行器 → 从容把层挂到首层上
/// （「挂层早于首层 settle」因此是确定的）→ 放行后首层 settle，后续层就地级联在同一帧里。
TEST(GateMacro_SameKindFallsThroughInline)
{
    CAsyncExecutor exec(1);  // 单线程 + 占住 worker：建链窗口内的时序完全确定
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    std::atomic<bool> bHold(false);
    std::atomic<bool> bRelease(false);
    ASSERT_TRUE(exec.Post(TaskKind::kWrite,
        [&bHold, &bRelease]()
        {
            bHold.store(true);
            asynctest::WaitFlag(bRelease, 3000);
        }));
    ASSERT_TRUE(asynctest::WaitFlag(bHold, 1000));

    // 唯一 worker 被占住 → 首层还没跑 → 现在挂层是确定的「早于 settle」
    CPromise<SGateCtx> chain = exec.NewPromise(spCtx, &StepCaptureFrame, TaskKind::kWrite, ASYNC_LOC).Then(&StepSameFrameGated);
    bRelease.store(true);

    ASSERT_TRUE(chain.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nRuns.load(), 1);                                   // 只跑一次（没有挂起 / 重入）
    ASSERT_EQ(spCtx->nKind.load(), static_cast<int>(TaskKind::kWrite));  // 且仍在首层那个写帧里（就地）
    ASSERT_TRUE(spCtx->pFrame.load() != NULL);                           // 首层确实在门内跑（有任务帧）
    exec.Stop();
}

/// @brief 调用点省略类别 = **不过门**（缺省 `kDirect`）：写者占着门时它照跑，且没有槽位。
TEST(GateMacro_OmittedKindRunsUngated)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    std::atomic<bool> bWriterHolds(false);
    std::atomic<bool> bReleaseWriter(false);
    std::atomic<bool> bRan(false);
    std::atomic<bool> bWriterStillHolding(false);

    // 写任务占住门不放
    ASSERT_TRUE(exec.Post(TaskKind::kWrite,
        [&bWriterHolds, &bReleaseWriter]()
        {
            bWriterHolds.store(true);
            asynctest::WaitFlag(bReleaseWriter, 2000);
        }));
    ASSERT_TRUE(asynctest::WaitFlag(bWriterHolds, 1000));

    CPromise<SGateCtx> chain = exec.NewPromise(
                                       spCtx,
                                       [](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                       {
                                           return CPromiseResult::Resolve();
                                       },
                                       TaskKind::kDirect, ASYNC_LOC)  // 首层显式直投（写者占门时也能跑）
                                   .Then(
                                       [&spCtx, &bRan, &bWriterStillHolding](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                       {
                                           RecordRun(spCtx);  // 省略类别 → 缺省不过门：帧应为 -1
                                           bWriterStillHolding.store(true);
                                           bRan.store(true);
                                           return CPromiseResult::Resolve();
                                       });  // ← 省略类别

    ASSERT_TRUE(asynctest::WaitFlag(bRan, 1000));  // 写者占门也照跑
    ASSERT_EQ(spCtx->nKind.load(), -1);            // 没有槽位（不过门）
    bReleaseWriter.store(true);
    ASSERT_TRUE(chain.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nRuns.load(), 1);
    exec.Stop();
}

/// @brief 请求 `kDirect`：直接落穿（不过门），不挂起。
TEST(GateMacro_DirectRequestIsNoOp)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    CPromise<SGateCtx> chain = exec.NewPromise(
                                       spCtx,
                                       [](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                       {
                                           return CPromiseResult::Resolve();
                                       },
                                       TaskKind::kDirect, ASYNC_LOC)  // 首层也直投：整条链都不占槽位
                                   .Then(&StepDirectRequest);         // 省略类别 + 体内 `ASYNC_GATE(kDirect)`

    ASSERT_TRUE(chain.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nRuns.load(), 1);
    ASSERT_EQ(spCtx->nKind.load(), -1);  // 直投：没有槽位
    exec.Stop();
}

// ====================================================================
// 二、升级 / 换档（挂起 → 过门重入）
// ====================================================================

/// @brief 声明写门 → 挂起 → 重入；**真等读者排空**，且期间函数体与下游层都不跑。
TEST(GateMacro_UpgradesToWriteAndWaitsReaders)
{
    CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    std::atomic<bool> bLongReadHolds(false);
    std::atomic<bool> bReleaseLongRead(false);
    std::atomic<bool> bFirstLayerRan(false);
    std::atomic<bool> bDownstream(false);

    // 长读任务占住读槽位（由主线程放行）
    ASSERT_TRUE(exec.Post(TaskKind::kRead,
        [&bLongReadHolds, &bReleaseLongRead]()
        {
            bLongReadHolds.store(true);
            asynctest::WaitFlag(bReleaseLongRead, 2000);
        }));
    ASSERT_TRUE(asynctest::WaitFlag(bLongReadHolds, 1000));

    CPromise<SGateCtx> chain = exec.NewPromise(
                                       spCtx,
                                       [&bFirstLayerRan](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                       {
                                           bFirstLayerRan.store(true);
                                           return CPromiseResult::Resolve();
                                       },
                                       TaskKind::kRead, ASYNC_LOC)
                                   .Then(&StepWriteGated)  // ← 省略类别：门要求由处理器自己声明（写）
                                   .Then(
                                       [&bDownstream](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                       {
                                           bDownstream.store(true);
                                           return CPromiseResult::Resolve();
                                       },
                                       TaskKind::kWrite, ASYNC_LOC);

    ASSERT_TRUE(asynctest::WaitFlag(bFirstLayerRan, 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_EQ(spCtx->nRuns.load(), 0);  // 读者没让位：升级的写层进不来
    ASSERT_TRUE(!bDownstream.load());   // 本层没 settle → 下游不提前跑

    bReleaseLongRead.store(true);
    ASSERT_TRUE(chain.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nRuns.load(), 1);                                   // 只跑一次（重入后宏落穿）
    ASSERT_EQ(spCtx->nKind.load(), static_cast<int>(TaskKind::kWrite));  // 在写槽位里
    ASSERT_EQ(spCtx->nStateCount.load(), 1);
    ASSERT_TRUE(bDownstream.load());
    exec.Stop();
}

/// @brief 声明读门 → 挂起 → 重入读槽位：与占门的读任务**并发**（读峰值 ≥ 2）。
TEST(GateMacro_ChangesToReadAndRunsConcurrently)
{
    CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    std::atomic<bool> bReleaseLongRead(false);

    std::atomic<bool> bLongReadInside(false);
    ASSERT_TRUE(exec.Post(TaskKind::kRead,
        [&spCtx, &bReleaseLongRead, &bLongReadInside]()
        {
            EnterReadSection(spCtx);
            bLongReadInside.store(true);
            asynctest::WaitFlag(bReleaseLongRead, 2000);
            LeaveReadSection(spCtx);
        }));
    ASSERT_TRUE(asynctest::WaitFlag(bLongReadInside, 1000));  // 长读已在读段里（重叠才是确定的）

    CPromise<SGateCtx> chain = exec.NewPromise(
                                       spCtx,
                                       [](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                       {
                                           return CPromiseResult::Resolve();
                                       },
                                       TaskKind::kDirect, ASYNC_LOC)
                                   .Then(&StepReadGated);  // ← 省略类别 + 体内声明读

    ASSERT_TRUE(chain.Await().IsFulfilled());  // 读门不排他：不用等长读结束就能落定
    bReleaseLongRead.store(true);
    ASSERT_EQ(spCtx->nKind.load(), static_cast<int>(TaskKind::kRead));
    ASSERT_TRUE(spCtx->nPeakReaders.load() >= 2);  // 与长读任务真的重叠了
    exec.Stop();
}

// ====================================================================
// 三、链语义不变（结果 / 顺序 / catch / finally / 首层）
// ====================================================================

/// @brief 挂起重入不改变结果：业务异常的类型与文案都被保真（且函数体只跑一次）。
TEST(GateMacro_RejectedResultTravelsWithType)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    CPromise<SGateCtx> chain = exec.NewPromise(
                                       spCtx,
                                       [](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                       {
                                           return CPromiseResult::Resolve();
                                       },
                                       TaskKind::kDirect, ASYNC_LOC)
                                   .Then(&StepWriteGatedReject);  // ← 省略类别 + 体内声明写

    const CPromiseResult result = chain.Await();
    ASSERT_TRUE(result.IsRejected());
    ASSERT_TRUE(dynamic_cast<const CGateMacroError*>(result.Exception().get()) != NULL);  // 类型保真
    ASSERT_EQ(result.Message(), std::string("声明写门后拒绝"));                           // 文案保真
    ASSERT_EQ(spCtx->nRuns.load(), 1);                                                    // 只跑一次
    ASSERT_EQ(spCtx->nKind.load(), static_cast<int>(TaskKind::kWrite));
    exec.Stop();
}

/// @brief catch 层也能自请过门：吞掉上游拒绝后链继续（后续层照常跑）。
TEST(GateMacro_WorksInCatchLayer)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    CPromise<SGateCtx> chain = exec.NewPromise(
                                       spCtx,
                                       [](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                       {
                                           return CPromiseResult::Reject(CGateMacroError("上游拒绝"));
                                       },
                                       TaskKind::kDirect, ASYNC_LOC)
                                   .Catch(&StepCatchGated)  // ← 省略类别 + 体内声明写（吞掉拒绝）
                                   .Then(&StepWriteGated);  // 恢复后继续（这层同样自请写门）

    ASSERT_TRUE(chain.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nRuns.load(), 2);                                   // 两层各跑一次（都没挂起第二次）
    ASSERT_EQ(spCtx->nKind.load(), static_cast<int>(TaskKind::kWrite));  // 两层都在写槽位里
    exec.Stop();
}

/// @brief finally 层也能自请过门：重入后仍「不改结果」（原拒绝 + 类型照旧）。
TEST(GateMacro_WorksInFinallyLayer)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    CPromise<SGateCtx> chain = exec.NewPromise(
                                       spCtx,
                                       [](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                       {
                                           return CPromiseResult::Reject(CGateMacroError("原样透传"));
                                       },
                                       TaskKind::kDirect, ASYNC_LOC)
                                   .Finally(&StepFinallyGated);  // ← 省略类别 + 体内声明写

    const CPromiseResult result = chain.Await();
    ASSERT_TRUE(result.IsRejected());
    ASSERT_TRUE(dynamic_cast<const CGateMacroError*>(result.Exception().get()) != NULL);
    ASSERT_EQ(result.Message(), std::string("原样透传"));
    ASSERT_EQ(spCtx->nRuns.load(), 1);
    ASSERT_EQ(spCtx->nKind.load(), static_cast<int>(TaskKind::kWrite));
    exec.Stop();
}

/// @brief 首层（`NewPromise` 的处理器）同样可以自请过门。
TEST(GateMacro_WorksInFirstLayer)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    const CPromiseResult result =
        exec.NewPromise(spCtx, &StepWriteGated, TaskKind::kRead, ASYNC_LOC)  // 首层声明读，体内要写 → 升级
            .Await();

    ASSERT_TRUE(result.IsFulfilled());
    ASSERT_EQ(spCtx->nRuns.load(), 1);
    ASSERT_EQ(spCtx->nKind.load(), static_cast<int>(TaskKind::kWrite));
    ASSERT_EQ(spCtx->nStateCount.load(), 1);
    exec.Stop();
}

// ====================================================================
// 四、重复挂起（宏：一个函数只允许一处，编译期拦下；漏网写法 → 第二次挂起即收口）
// ====================================================================

/// @brief 同一类别的重复请求（函数形态，绕过宏）：第二处落穿，函数体只跑一次。
TEST(GateMacro_SameKindRepeatedRequestIsNoop)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    CPromise<SGateCtx> chain = exec.NewPromise(
                                       spCtx,
                                       [](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                       {
                                           return CPromiseResult::Resolve();
                                       },
                                       TaskKind::kDirect, ASYNC_LOC)
                                   .Then(&StepSameKindTwiceRequests);  // ← 省略类别 + 体内两次请求「读」

    ASSERT_TRUE(chain.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nRuns.load(), 1);
    ASSERT_EQ(spCtx->nKind.load(), static_cast<int>(TaskKind::kRead));
    exec.Stop();
}

/// @brief 兜底（绕过宏、同一函数里先后请求两种类别）：**第二次挂起即收口** —— 有界失败，不来回换档。
TEST(GateMacro_SuspendedTwiceIsBounded)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    CPromise<SGateCtx> chain = exec.NewPromise(
                                       spCtx,
                                       [](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                       {
                                           return CPromiseResult::Resolve();
                                       },
                                       TaskKind::kDirect, ASYNC_LOC)
                                   .Then(&StepMixedKindRequests);  // ← 省略类别 + 体内先后请求读、写

    const CPromiseResult result = chain.Await();  // 挂死的话这里会超时（用例失败），不会卡死进程
    ASSERT_TRUE(result.IsRejected());
    ASSERT_TRUE(asynctest::IsGateSuspendedTwiceFailure(result));
    ASSERT_EQ(spCtx->nRuns.load(), 0);  // 层体从未真正跑完（每次都在请求那一行被挂起）
    exec.Stop();
}

/// @brief 兜底（辅助 lambda 换档）：第二次挂起即收口 —— 与上例同一条兜底路径，另一种漏网写法。
TEST(GateMacro_NestedHelperSwitchIsBounded)
{
    CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    CPromise<SGateCtx> chain = exec.NewPromise(
                                       spCtx,
                                       [](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                       {
                                           return CPromiseResult::Resolve();
                                       },
                                       TaskKind::kDirect, ASYNC_LOC)
                                   .Then(&StepOuterWithGatedHelper);  // ← 省略类别 + 体内「外层写、内层读」

    const CPromiseResult result = chain.Await();  // 挂死的话这里会超时（用例失败），不会卡死进程
    ASSERT_TRUE(result.IsRejected());
    ASSERT_TRUE(asynctest::IsGateSuspendedTwiceFailure(result));
    ASSERT_EQ(spCtx->nRuns.load(), 0);  // 辅助 lambda 从未跑完（每次都在它那行被挂起）
    exec.Stop();
}

// ====================================================================
// 五、并发正确性（与模块自己的任务共存）
// ====================================================================

/// @brief 声明写门的层与模块自己的写任务并发：**零重叠** + 计数精确。
TEST(GateMacro_SerializesWithModuleWriters)
{
    const int kFlows = 40;

    CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();
    std::vector<CPromise<SGateCtx> > vecChains;
    for (int i = 0; i < kFlows; ++i)
    {
        CPromise<SGateCtx> chain = exec.NewPromise(
                                           spCtx,
                                           [](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                           {
                                               return CPromiseResult::Resolve();
                                           },
                                           TaskKind::kDirect, ASYNC_LOC)
                                       .Then(&StepWriteGated);  // ← 省略类别 + 体内声明写
        vecChains.push_back(chain);

        // 模块自己的写任务（同一份状态）
        ASSERT_TRUE(exec.Post(TaskKind::kWrite,
            [&spCtx]()
            {
                EnterWriteSection(spCtx);
                spCtx->nStateCount.fetch_add(1);
                LeaveWriteSection(spCtx);
            }));
    }

    for (std::size_t i = 0; i < vecChains.size(); ++i)
    {
        ASSERT_TRUE(vecChains[i].Await().IsFulfilled());
    }
    exec.Stop();

    ASSERT_EQ(spCtx->nStateCount.load(), 2 * kFlows);  // 声明写门的层 + 写任务，各一次
    ASSERT_EQ(spCtx->nOverlaps.load(), 0);             // 状态段任意时刻只有一个进入者
    ASSERT_EQ(spCtx->nRuns.load(), kFlows);            // 每个层体只跑一次
}

/// @brief 声明读门的层并发跑：读段峰值 ≥ 2（没有被串行化）。
TEST(GateMacro_ReaderDeclarationsRunConcurrently)
{
    const int kFlows = 8;

    CAsyncExecutor exec(4);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<SGateCtx> spCtx = std::make_shared<SGateCtx>();

    // 先放一个「长读任务」稳稳占住读段：随后每条声明读门的层都要与它重叠 —— 峰值 ≥ 2 是**确定**的，
    // 不依赖调度运气。
    std::atomic<bool> bLongReadInside(false);
    std::atomic<bool> bReleaseLongRead(false);
    ASSERT_TRUE(exec.Post(TaskKind::kRead,
        [&spCtx, &bLongReadInside, &bReleaseLongRead]()
        {
            EnterReadSection(spCtx);
            bLongReadInside.store(true);
            asynctest::WaitFlag(bReleaseLongRead, 3000);
            LeaveReadSection(spCtx);
        }));
    ASSERT_TRUE(asynctest::WaitFlag(bLongReadInside, 1000));

    std::vector<CPromise<SGateCtx> > vecChains;
    for (int i = 0; i < kFlows; ++i)
    {
        CPromise<SGateCtx> chain = exec.NewPromise(
                                           spCtx,
                                           [](const std::shared_ptr<SGateCtx>& /*spCtx*/)
                                           {
                                               return CPromiseResult::Resolve();
                                           },
                                           TaskKind::kDirect, ASYNC_LOC)
                                       .Then(&StepReadGated);  // ← 省略类别 + 体内声明读
        vecChains.push_back(chain);
    }

    for (std::size_t i = 0; i < vecChains.size(); ++i)
    {
        ASSERT_TRUE(vecChains[i].Await().IsFulfilled());
    }
    bReleaseLongRead.store(true);  // 读门不排他：这 8 条不用等长读结束就落定了
    exec.Stop();

    ASSERT_TRUE(spCtx->nPeakReaders.load() >= 2);  // 读真的并发了（与长读任务重叠）
    ASSERT_EQ(spCtx->nRuns.load(), kFlows);
    ASSERT_EQ(spCtx->nKind.load(), static_cast<int>(TaskKind::kRead));
}
