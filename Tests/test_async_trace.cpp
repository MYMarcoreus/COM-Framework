/// @file test_async_trace.cpp
/// 异步调用链（Trace）：在层处理器内部看到「我这一层 + 上游链」。
///
/// 被测契约：
///  - `VisitLayerChain` 从当前层往上游走（近 → 远），深度、模式、注册点都对得上；
///  - `CurrentLayer()` 在层处理器里返回本层，在层外返回 nullptr；
///  - `DescribeLayerChain()` 在层里能拼出可读的一行；
///  - 开关与注册点 `ASYNC_LOC` 同一个（调试构建 `ASYNC_DEBUG_TRACE`）：
///    发布构建下以上接口一律是空操作（本文件的用例在两种构建下都跑）。
///
/// 注：`CLayerInfo` 里的 `loc` 指向编译期静态串，可以直接拷出来断言；
///     但 `CurrentLayer()` 返回的是 thread_local 存储，必须**立刻拷贝**。

#include <stdexcept>
#include <string>
#include <vector>

#include "Async/Promise.h"
#include "Async/Trace.h"
#include "TestFramework.h"

namespace {

/// 测试用共享上下文（顺带当采集箱）。
struct CTraceCtx
{
    int nValue;                                       ///< 逐层累加，用于确认链真的跑过。
    bool bHasCurrent;                                 ///< 层里 `CurrentLayer()` 是否非空。
    common::async::CLayerInfo infoCurrent;            ///< 层里 `CurrentLayer()` 的快照（拷贝）。
    bool bVisited;                                    ///< `VisitLayerChain` 的返回值。
    std::vector<common::async::CLayerInfo> vecChain;  ///< 采集到的链（近 → 远）。
    std::string strChain;                             ///< `DescribeLayerChain()` 的结果。

    CTraceCtx() : nValue(0), bHasCurrent(false), infoCurrent(), bVisited(false), vecChain(), strChain()
    {}
};

/// 在层处理器里采集「当前层 + 上游链」。
void CollectChain(const std::shared_ptr<CTraceCtx>& spCtx)
{
    spCtx->vecChain.clear();  // 每次采集都是新快照（同一层/不同层可能采多次）
    const common::async::CLayerInfo* pCurrent = common::async::CurrentLayer();
    spCtx->bHasCurrent = (pCurrent != NULL);
    if (pCurrent != NULL)
    {
        spCtx->infoCurrent = *pCurrent;  // TLS 存储 → 必须立刻拷贝
    }

    spCtx->bVisited = common::async::VisitLayerChain(
        [spCtx](const common::async::CLayerInfo& info)
        {
            spCtx->vecChain.push_back(info);
        });
    spCtx->strChain = common::async::DescribeLayerChain();
}

// 层：只累加（用于把链拉长）。
common::async::CPromiseResult StepBump(common::async::CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    ++spCtx->nValue;
    return common::async::CPromiseResult::Resolve();
}

/// 层：采集链。**原样透传**上一层结果 —— 采集（观察）不该改变链的走向，
/// 这样它放在 then / catch / finally 任何位置都安全（放 catch 位置时返回 `Resolve()`
/// 会把拒绝吞掉，链就"恢复"了，用例的预期也会跟着变）。
///
/// 注意：这里**不能**写 `if (upResult.IsRejected()) return upResult;` 这种 then 式防御 ——
/// catch 层一定会看到拒绝，带了判断就什么都采集不到（写这个用例时踩过一次）。
common::async::CPromiseResult StepCollect(common::async::CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    CollectChain(spCtx);
    return upResult;
}

// 层：采集链后故意抛异常（验证异常路径下帧栈也能正确弹回）。
common::async::CPromiseResult StepCollectThenThrow(
    common::async::CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    CollectChain(spCtx);
    throw std::runtime_error("trace 用例：故意抛异常");
}

// 层：拒绝（用于让 catch 层真正执行）。
common::async::CPromiseResult StepReject(common::async::CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    (void)spCtx;
    return common::async::CPromiseResult::Reject(common::async::kBusinessBase + 1);
}

}  // namespace

/// @brief 三层链：在最后一层里能看到「自己 + 上游两层」，深度与注册点都对得上。
TEST(Trace_ChainVisibleInsideLayer)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTraceCtx> spCtx = std::make_shared<CTraceCtx>();

    const int nLineHead = __LINE__ + 1;
    common::async::CPromise<CTraceCtx> pHead = exec.NewPromise(spCtx, &StepBump, ASYNC_LOC);
    const int nLineMid = __LINE__ + 1;
    common::async::CPromise<CTraceCtx> pMid = pHead.Then(&StepBump, ASYNC_LOC);
    const int nLineTail = __LINE__ + 1;
    common::async::CPromise<CTraceCtx> pTail = pMid.Then(&StepCollect, ASYNC_LOC);

    ASSERT_TRUE(pTail.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 2);  // 前两层各 +1（第三层只采集）

#if defined(ASYNC_DEBUG_TRACE)
    ASSERT_TRUE(spCtx->bVisited);
    ASSERT_TRUE(spCtx->bHasCurrent);
    ASSERT_EQ(spCtx->vecChain.size(), static_cast<size_t>(3));

    // 近 → 远：本层 → 中层 → 首层；"是不是当前层" 只有第一条为真。
    ASSERT_TRUE(spCtx->vecChain[0].bCurrent);
    ASSERT_TRUE(!spCtx->vecChain[1].bCurrent);
    ASSERT_TRUE(!spCtx->vecChain[2].bCurrent);
    ASSERT_EQ(spCtx->vecChain[0].nDepth, 0);
    ASSERT_EQ(spCtx->vecChain[1].nDepth, 1);
    ASSERT_EQ(spCtx->vecChain[2].nDepth, 2);

    // 注册点：就是三个挂层语句各自的 __LINE__（ASYNC_LOC）。
    ASSERT_EQ(spCtx->vecChain[0].loc.nLine, nLineTail);
    ASSERT_EQ(spCtx->vecChain[1].loc.nLine, nLineMid);
    ASSERT_EQ(spCtx->vecChain[2].loc.nLine, nLineHead);
    ASSERT_EQ(spCtx->vecChain[0].loc.nLine, spCtx->infoCurrent.loc.nLine);  // CurrentLayer 与链首一致
    ASSERT_EQ(spCtx->infoCurrent.nDepth, 0);
    ASSERT_TRUE(spCtx->infoCurrent.bCurrent);

    // 注册点文件就是本文件（避免把别的测试文件的链误认成自己的）。
    const std::string strFile = spCtx->vecChain[0].loc.szFile != NULL ? spCtx->vecChain[0].loc.szFile : "";
    ASSERT_TRUE(strFile.find("test_async_trace.cpp") != std::string::npos);

    // 一行描述：含本层函数名与文件行号。
    ASSERT_TRUE(spCtx->strChain.find("test_async_trace.cpp") != std::string::npos);
    ASSERT_TRUE(spCtx->strChain.find("#0 then") != std::string::npos);
    ASSERT_TRUE(spCtx->strChain.find("#2 then") != std::string::npos);
#else
    // 发布构建：trace 按契约是空操作（与 ASYNC_LOC 同一个开关）。
    (void)nLineHead;  // 只在调试分支里用于断言行号
    (void)nLineMid;
    (void)nLineTail;
    ASSERT_TRUE(!spCtx->bVisited);
    ASSERT_TRUE(!spCtx->bHasCurrent);
    ASSERT_TRUE(spCtx->vecChain.empty());
    ASSERT_TRUE(spCtx->strChain.empty());
#endif

    exec.Stop();
}

/// @brief 模式与深度：then → catch → finally 全都记进了链里。
TEST(Trace_ModeIsRecorded)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTraceCtx> spCtx = std::make_shared<CTraceCtx>();
    common::async::CPromise<CTraceCtx> pTail = exec.NewPromise(spCtx, &StepReject, ASYNC_LOC)  // 首层：拒绝
                                                   .Then(&StepBump, ASYNC_LOC)                 // 失败即停（不执行）
                                                   .Catch(&StepCollect, ASYNC_LOC)             // 采集（模式 catch）
                                                   .Finally(&StepCollect, ASYNC_LOC);          // 再采集（模式 finally）

    ASSERT_TRUE(pTail.Await().IsRejected());  // 拒绝码原样透传

#if defined(ASYNC_DEBUG_TRACE)
    ASSERT_TRUE(spCtx->bVisited);
    ASSERT_EQ(spCtx->vecChain.size(), static_cast<size_t>(4));
    ASSERT_TRUE(spCtx->vecChain[0].eMode == common::async::detail::kModeFinally);
    ASSERT_TRUE(spCtx->vecChain[1].eMode == common::async::detail::kModeCatch);
    ASSERT_TRUE(spCtx->vecChain[2].eMode == common::async::detail::kModeThen);  // 被跳过的 then 层也在链上
    ASSERT_TRUE(spCtx->vecChain[3].eMode == common::async::detail::kModeThen);  // 首层
    ASSERT_EQ(spCtx->vecChain[2].nDepth, 2);
    ASSERT_TRUE(spCtx->strChain.find("#0 finally") != std::string::npos);
    ASSERT_TRUE(spCtx->strChain.find("#1 catch") != std::string::npos);
#else
    ASSERT_TRUE(!spCtx->bVisited);
#endif

    exec.Stop();
}

/// @brief 分叉：每条分支都能看到「自己 + 共同的上一层」。
TEST(Trace_ForkedBranchSeesUpstream)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTraceCtx> spCtx = std::make_shared<CTraceCtx>();
    common::async::CPromise<CTraceCtx> pHead = exec.NewPromise(spCtx, &StepBump, ASYNC_LOC);
    common::async::CPromise<CTraceCtx> pBranchA = pHead.Then(&StepCollect, ASYNC_LOC);  // 分支 A：采集
    common::async::CPromise<CTraceCtx> pBranchB = pHead.Then(&StepBump, ASYNC_LOC);     // 分支 B：只累加

    ASSERT_TRUE(pBranchA.Await().IsFulfilled());
    ASSERT_TRUE(pBranchB.Await().IsFulfilled());

#if defined(ASYNC_DEBUG_TRACE)
    ASSERT_TRUE(spCtx->bVisited);
    ASSERT_EQ(spCtx->vecChain.size(), static_cast<size_t>(2));  // 分支层 + 首层
    ASSERT_TRUE(spCtx->vecChain[0].bCurrent);
    ASSERT_TRUE(!spCtx->vecChain[1].bCurrent);
    ASSERT_EQ(spCtx->vecChain[1].nDepth, 1);
#else
    ASSERT_TRUE(!spCtx->bVisited);
#endif

    exec.Stop();
}

/// @brief 层外调用：不是层 → 拿不到当前层，遍历返回 false（不崩）。
TEST(Trace_NotInsideLayer)
{
    ASSERT_TRUE(common::async::CurrentLayer() == NULL);
    ASSERT_TRUE(!common::async::VisitLayerChain(
        [](const common::async::CLayerInfo&)
        {
        }));

    common::async::CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTraceCtx> spCtx = std::make_shared<CTraceCtx>();
    ASSERT_TRUE(exec.NewPromise(spCtx, &StepBump, ASYNC_LOC).Await().IsFulfilled());

    // 层跑在 worker 线程上；主线程（调用方）始终不在层里。
    ASSERT_TRUE(common::async::CurrentLayer() == NULL);
    ASSERT_TRUE(common::async::DescribeLayerChain().empty());
    exec.Stop();
}

/// @brief 层里抛异常：帧栈照样弹回（异常之后再调，仍然是「不在层里」）。
TEST(Trace_FramePoppedAfterThrow)
{
    common::async::CAsyncExecutor exec(2);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTraceCtx> spCtx = std::make_shared<CTraceCtx>();
    const common::async::CPromiseResult r =
        exec.NewPromise(spCtx, &StepCollectThenThrow, ASYNC_LOC).Catch(&StepBump, ASYNC_LOC).Await();

    ASSERT_TRUE(r.IsRejected());
    ASSERT_TRUE(r.Code() == static_cast<int>(common::async::kException));  // 层内异常 → kException

#if defined(ASYNC_DEBUG_TRACE)
    ASSERT_TRUE(spCtx->bVisited);                               // 抛之前已经采集到了
    ASSERT_EQ(spCtx->vecChain.size(), static_cast<size_t>(1));  // 只有它自己（首层无上游）
#endif

    // 异常路径后当前线程的帧栈必须已弹空（否则后面的遍历会看到残留的层）。
    ASSERT_TRUE(common::async::CurrentLayer() == NULL);
    exec.Stop();
}
