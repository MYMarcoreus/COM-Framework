/// @file test_async_trace.cpp
/// 异步调用链（trace）：验证「在异步层里能不能看到**完整**的调用链」。
///
/// 主用例 `Trace_CompleteChainInComplexFlow` 是一条把各种层形态混在一起的主链：
/// ① 具名 then（链根）→ ② 具名 then → ③ `ThenInline` → ④ `ThenOn` 别的执行器
/// → ⑤ 被跳过的 `Catch` → ⑥ `Finally` → ⑦ `ThenPromise` 内层链 → ⑧ 分叉基座
/// → ⑨ 两支；然后在**最深的地方**把整条链逐层断言出来（层数、模式、注册点行号、
/// 深度、当前层标记、一行描述），另外把特殊位置逐个钉住：
///   - 子链 → 父链：内层链（`ThenPromise`）的链根挂在**起它的那一层**下面 → 从内层里能一路
///     追回主链；反过来主链看不到子链（只往上游走）；层外起的链没有父层；
///   - 分叉：每条分支只看得到「自己 + 共同上游」，看不到兄弟分支；
///   - 通知（`OnSettled`）：落定前登记 → 在**触发它的那一层**的帧里就地执行；
///     落定后才登记 → 投递执行，此时不在任何层里（通知不是层）；
///   - 协程：`CO_AWAIT` 等的是自己起的子链（独立一条）；恢复点是否在层里取决于
///     就地 / 投递续跑（两种都合法，用例断言这个上界）；
///   - 层内抛异常：帧栈照样弹回（异常之后仍然是「不在层里」）。
///
/// 开关与注册点 `ASYNC_LOC` 同一个（调试构建 `ASYNC_DEBUG_TRACE`）：
/// 发布构建下所有接口一律是空操作（本文件的用例在两种构建下都跑）。
///
/// 注：`CLayerInfo.loc` 指向编译期静态串，可以拷出来断言；但 `CurrentLayer()`
///     返回的是 thread_local 存储，必须**立刻拷贝**。

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Async/Promise.h"
#include "Async/Trace.h"
#include "Coroutine/Coroutine.h"
#include "TestFramework.h"

using common::async::CAsyncExecutor;
using common::async::CCoroutine;
using common::async::CPromise;
using common::async::CPromiseResult;

#if defined(ASYNC_DEBUG_TRACE)
using common::async::CLayerInfo;  // 只有调试构建有 trace 类型
#endif

namespace {

#if defined(ASYNC_DEBUG_TRACE)

/// @brief 一个「位置」采集到的东西（在层处理器里现场取一份快照）。
///
/// 只在调试构建定义：发布构建没有 `CLayerInfo`（trace 整段不存在），也没有可采集的东西。
struct CCapture
{
    bool bHasCurrent;                  ///< `CurrentLayer()` 是否非空。
    CLayerInfo infoCurrent;            ///< `CurrentLayer()` 的快照（拷贝）。
    bool bVisited;                     ///< `VisitLayerChain()` 的返回值。
    std::vector<CLayerInfo> vecChain;  ///< 采集到的链（近 → 远）。
    std::string strChain;              ///< `DescribeLayerChain()` 的结果。

    CCapture() : bHasCurrent(false), infoCurrent(), bVisited(false), vecChain(), strChain()
    {}
};

#endif  // defined(ASYNC_DEBUG_TRACE)

/// @brief 期望的链：逐层的（模式, 注册点行号）。
struct CExpect
{
    const char* pszMode;  ///< then / catch / finally。
    int nLine;            ///< 注册点行号（`__LINE__ + 1` 采集）。
};

/// @brief 主链各层的注册点（每个 `__LINE__ + 1` 紧跟一次挂层）。
///
/// 放在一个结构里是为了发布构建下只需一句 `(void)lines;` —— trace 关掉时它们没有用处。
struct CLines
{
    int nRoot;     ///< `exec.NewPromise`（链根）
    int nInline;   ///< `ThenInline`
    int nOther;    ///< `ThenOn`（另一个执行器）
    int nCatch;    ///< `Catch`（本流程被跳过，但仍在链上）
    int nFinally;  ///< `Finally`
    int nBridge;   ///< `ThenPromise`（内层链挂在主链上的那一层）
    int nBase;     ///< 分叉基座
    int nBranchA;  ///< 分叉分支 A
    int nBranchB;  ///< 分叉分支 B
    int nThrow;    ///< 异常路径那条小链的首层

    CLines() : nRoot(0), nInline(0), nOther(0), nCatch(0), nFinally(0), nBridge(0), nBase(0), nBranchA(0), nBranchB(0), nThrow(0)
    {}
};

/// @brief 测试用共享上下文（顺带当采集箱：每个「位置」一个槽位）。
///
/// 一条链上的层是顺序执行的，所以槽位不会有并发写；分叉两支各写各的槽位
/// （「并行分支只写不同字段」是本框架的约定）。
struct CTraceCtx
{
    int nValue;                     ///< 主链累加（确认链真的跑过）
    std::atomic<bool> bCatchRan;    ///< 被跳过的 Catch 层是否执行了（应为 false）
    std::atomic<bool> bFinallyRan;  ///< Finally 是否执行了（应为 true）
    std::atomic<bool> bGateOpened;  ///< 门：主线程放行后，被挡住的那一层才落定
    std::atomic<bool> bNoticeDone;  ///< 「落定前登记」的通知已送达
    std::atomic<bool> bPostedDone;  ///< 「落定后登记」的通知已送达

    // 下面这些只在调试构建存在（发布构建根本没有 trace：类型与函数整段不参与编译）。
#if defined(ASYNC_DEBUG_TRACE)
    int nLineInnerFirst;   ///< 内层链第 1 层注册点（在内层链的工厂里采集）
    int nLineInnerSecond;  ///< 内层链第 2 层注册点
    int nLineCoroStep;     ///< 协程里 await 的那条子链的注册点
    int nLineStart;        ///< 层外起链时（`exec.NewPromise`）的注册点

    CCapture capRoot;          ///< 链根
    CCapture capInline;        ///< `ThenInline` 层
    CCapture capOtherExec;     ///< `ThenOn` 层（跑在另一个执行器上）
    CCapture capDeepest;       ///< 分支 B：主链最深，看整条链
    CCapture capBranchA;       ///< 分支 A：看「自己 + 共同上游」
    CCapture capInnerFirst;    ///< 内层链第 1 层
    CCapture capInnerSecond;   ///< 内层链第 2 层（内层最深）
    CCapture capNoticeInline;  ///< 通知：落定前登记（就地送达）
    CCapture capNoticePosted;  ///< 通知：落定后登记（投递送达）
    CCapture capCoroStep;      ///< 协程 `CO_AWAIT` 等的那条子链的层
    CCapture capCoroAfter;     ///< 协程体：`CO_AWAIT` 返回之后
    CCapture capThrowing;      ///< 抛异常那一层（抛之前采集）
#endif

    CTraceCtx()
        : nValue(0),
          bCatchRan(false),
          bFinallyRan(false),
          bGateOpened(false),
          bNoticeDone(false),
          bPostedDone(false)
#if defined(ASYNC_DEBUG_TRACE)
          ,
          nLineInnerFirst(0),
          nLineInnerSecond(0),
          nLineCoroStep(0),
          nLineStart(0),
          capRoot(),
          capInline(),
          capOtherExec(),
          capDeepest(),
          capBranchA(),
          capInnerFirst(),
          capInnerSecond(),
          capNoticeInline(),
          capNoticePosted(),
          capCoroStep(),
          capCoroAfter(),
          capThrowing()
#endif
    {}
};

#if defined(ASYNC_DEBUG_TRACE)

/// @brief 采集「当前层 + 上游链」的现场快照（在层处理器里调用）。
void CaptureNow(CCapture& cap)
{
    cap.vecChain.clear();  // 每次都是新快照（同一槽位可能被采集多次）
    const CLayerInfo* pCurrent = common::async::CurrentLayer();
    cap.bHasCurrent = (pCurrent != NULL);
    if (pCurrent != NULL)
    {
        cap.infoCurrent = *pCurrent;  // TLS 存储 → 必须立刻拷贝
    }

    cap.bVisited = common::async::VisitLayerChain(
        [&cap](const CLayerInfo& info)
        {
            cap.vecChain.push_back(info);
        });
    cap.strChain = common::async::DescribeLayerChain();
}

/// @brief 层模式的文本形式（与 `DescribeLayerChain()` 的写法一致）。
///
/// 下面这些助手只在调试构建有 trace 时用得上，所以整段跟着开关走。
const char* ModeText(common::async::detail::HandlerMode eMode)
{
    if (eMode == common::async::detail::kModeCatch)
    {
        return "catch";
    }
    if (eMode == common::async::detail::kModeFinally)
    {
        return "finally";
    }
    return "then";
}

/// @brief 断言采集到的链与期望**逐项**一致（近 → 远）。
///
/// 逐项检查模式 / 注册点行号 / 深度（第 i 项的深度必须是 i）/ 当前层标记
/// （只有第 0 项为真）；最后再确认 `CurrentLayer()` 与链首是同一层。
void AssertChain(const CCapture& cap, const CExpect* pExpect, int nCount)
{
    ASSERT_TRUE(cap.bVisited);
    ASSERT_TRUE(cap.bHasCurrent);
    ASSERT_EQ(cap.vecChain.size(), static_cast<size_t>(nCount));
    for (int i = 0; i < nCount && i < static_cast<int>(cap.vecChain.size()); ++i)
    {
        const CLayerInfo& info = cap.vecChain[static_cast<size_t>(i)];
        ASSERT_TRUE(std::string(ModeText(info.eMode)) == pExpect[i].pszMode);
        ASSERT_EQ(info.loc.nLine, pExpect[i].nLine);
        ASSERT_EQ(info.nDepth, i);
        ASSERT_TRUE(info.bCurrent == (i == 0));
    }

    ASSERT_TRUE(cap.infoCurrent.bCurrent);
    ASSERT_EQ(cap.infoCurrent.loc.nLine, pExpect[0].nLine);
    ASSERT_EQ(cap.infoCurrent.nDepth, 0);
}

/// @brief 链里有没有某个注册点（用来断言「看不到某一层」）。
bool HasLine(const CCapture& cap, int nLine)
{
    for (size_t i = 0; i < cap.vecChain.size(); ++i)
    {
        if (cap.vecChain[i].loc.nLine == nLine)
        {
            return true;
        }
    }
    return false;
}

/// @brief 一行描述里的箭头个数（= 链上层数 − 1）。
int CountArrows(const std::string& strChain)
{
    int nCount = 0;
    for (size_t nPos = strChain.find(" <- "); nPos != std::string::npos; nPos = strChain.find(" <- ", nPos + 1))
    {
        ++nCount;
    }
    return nCount;
}
#endif  // defined(ASYNC_DEBUG_TRACE)

/// 采集一步（调试构建）；发布构建没有 trace，这一步不参与。
///
/// 写成宏是为了让两份构建共用同一套处理器代码（否则每个处理器里都要写一段 `#if`）。
#if defined(ASYNC_DEBUG_TRACE)
    #define TRACE_CAPTURE(member) CaptureNow(spCtx->member)
#else
    /// 发布构建：没有 trace 可采（顺手把形参用掉，免得 `-Wunused-parameter`）。
    #define TRACE_CAPTURE(member) ((void)spCtx)
#endif

/// 记一个「注册点行号」（只有调试构建需要：发布构建既没有 trace，也就没有行号可比）。
///
/// `__LINE__` 在宏调用那一行展开 → `+1` 就是紧接着的挂层语句，两份构建都不受影响。
#if defined(ASYNC_DEBUG_TRACE)
    #define TRACE_LINE(assign) assign
#else
    #define TRACE_LINE(assign) ((void)0)
#endif

/// @brief 等一个由工作线程置位的标志（有上限；超时返回 false —— 别让用例挂死）。
bool WaitFlag(const std::atomic<bool>& bFlag, int nMaxMs = 3000)
{
    const int nStepMs = 5;
    for (int i = 0; i < nMaxMs / nStepMs; ++i)
    {
        if (bFlag.load(std::memory_order_acquire))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(nStepMs));
    }
    return bFlag.load(std::memory_order_acquire);
}

//================ 主链上的层（每个层采集自己的位置） ================

/// 层：主链链根（顺带累加，确认链真的跑过）。顺带当异常路径那条链的 Catch 用。
CPromiseResult StepRoot(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    TRACE_CAPTURE(capRoot);
    ++spCtx->nValue;
    return CPromiseResult::Resolve();
}

/// 层：`ThenInline`（就地执行）。
CPromiseResult StepInline(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    TRACE_CAPTURE(capInline);
    return CPromiseResult::Resolve();
}

/// 层：`ThenOn`（跑在另一个执行器上 —— 换线程不影响链的可见性）。
CPromiseResult StepOnOtherExec(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    TRACE_CAPTURE(capOtherExec);
    return CPromiseResult::Resolve();
}

/// 层：`Catch`（本流程上游兑现 → 这一层被跳过；但它照样在链上，见 `capDeepest`）。
CPromiseResult StepCatchSkipped(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    spCtx->bCatchRan = true;
    return CPromiseResult::Resolve();
}

/// 层：`Finally`（不改结果，原样透传）。
CPromiseResult StepFinally(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    spCtx->bFinallyRan = true;
    return CPromiseResult::Resolve();
}

/// 层：内层链第 1 层。
CPromiseResult StepInnerFirst(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    TRACE_CAPTURE(capInnerFirst);
    return CPromiseResult::Resolve();
}

/// 层：内层链第 2 层（内层最深 —— 这里只看得到内层链自己）。
CPromiseResult StepInnerSecond(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    TRACE_CAPTURE(capInnerSecond);
    return CPromiseResult::Resolve();
}

/// 层：分叉基座（什么也不做，只为了让两支有共同的上游）。
CPromiseResult StepForkBase(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    (void)spCtx;
    return CPromiseResult::Resolve();
}

/// 层：分叉分支 A。
CPromiseResult StepBranchA(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    TRACE_CAPTURE(capBranchA);
    return CPromiseResult::Resolve();
}

/// 层：分叉分支 B（主链最深 —— 在这里看整条链）。
CPromiseResult StepBranchB(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    TRACE_CAPTURE(capDeepest);
    return CPromiseResult::Resolve();
}

/// 层：被门挡住（等主线程放行才落定）—— 让「登记通知」稳稳地发生在 settle 之前。
///
/// 用例要验证的是「落定前登记的通知在**触发层**的帧里就地执行」：若不等门，
/// 层可能在主线程登记通知之前就落定了，那条路径会变成「落定后登记 → 投递」。
CPromiseResult StepGated(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    for (int i = 0; i < 4000 && !spCtx->bGateOpened.load(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return CPromiseResult::Resolve();
}

/// 层：协程里 `CO_AWAIT` 等的那条子链的层。
CPromiseResult StepCoroutineStep(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    TRACE_CAPTURE(capCoroStep);
    return CPromiseResult::Resolve();
}

/// 层：采集后故意抛异常（验证异常路径下帧栈照样弹回）。
CPromiseResult StepCaptureThenThrow(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    TRACE_CAPTURE(capThrowing);
    throw std::runtime_error("trace 用例：层内故意抛异常");
}

/// 层：then 式透传（给上面的异常链当 Catch：把 kException 原样透传出去）。
CPromiseResult StepPassThrough(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)spCtx;
    return upResult;
}

/// 协程：`CO_AWAIT` 等一条自己起的子链，恢复后再采集一次（看恢复点在不在层里）。
class CProbeCoroutine : public CCoroutine<CTraceCtx>
{
public:
    explicit CProbeCoroutine(const std::shared_ptr<CTraceCtx>& spCtx) : CCoroutine<CTraceCtx>(spCtx)
    {}

    void Run() override
    {
        CO_BEGIN();
        TRACE_LINE(GetContext()->nLineCoroStep = __LINE__ + 1);  // 下一行（CO_AWAIT）才是注册点
        CO_AWAIT(NewPromise(&StepCoroutineStep, ASYNC_LOC));
#if defined(ASYNC_DEBUG_TRACE)
        CaptureNow(GetContext()->capCoroAfter);  // 恢复点：看还在不在层里
#endif
        CO_RETURN_VOID();
        CO_END();
    }
};

}  // namespace

/// @brief 复杂主链：所有层形态串成一条链，在最深处断言**完整**调用链 + 各种特殊位置。
TEST(Trace_CompleteChainInComplexFlow)
{
    CAsyncExecutor execMain(2);  // 主链：2 线程，分叉两支正好一支就地、一支投递
    CAsyncExecutor execSide(1);  // `ThenOn` 指定的另一个执行器（换线程）
    ASSERT_TRUE(execMain.Start());
    ASSERT_TRUE(execSide.Start());

    std::shared_ptr<CTraceCtx> spCtx = std::make_shared<CTraceCtx>();
    CLines lines;

    // 内层链的工厂（单独具名：这样 `ThenPromise` 那一行能整行写下，注册点行号好断言）。
    CPromise<CTraceCtx>::PromiseFactory fnInnerChain = [&execMain, spCtx](const std::shared_ptr<CTraceCtx>& spInnerCtx)
    {
        TRACE_LINE(spCtx->nLineInnerFirst = __LINE__ + 1);
        CPromise<CTraceCtx> pInner = execMain.NewPromise(spInnerCtx, &StepInnerFirst, ASYNC_LOC);
        TRACE_LINE(spCtx->nLineInnerSecond = __LINE__ + 1);
        return pInner.Then(&StepInnerSecond, ASYNC_LOC);
    };

    //---------------- 主链：一条「下单」流程，把层形态混起来 ----------------

    lines.nRoot = __LINE__ + 1;
    CPromise<CTraceCtx> pRoot = execMain.NewPromise(spCtx, &StepRoot, ASYNC_LOC);  // ① 链根
    lines.nInline = __LINE__ + 1;
    CPromise<CTraceCtx> pInline = pRoot.ThenInline(&StepInline, ASYNC_LOC);  // ② 就地
    lines.nOther = __LINE__ + 1;
    CPromise<CTraceCtx> pOnSide = pInline.ThenOn(execSide, &StepOnOtherExec, ASYNC_LOC);  // ③ 换执行器
    lines.nCatch = __LINE__ + 1;
    CPromise<CTraceCtx> pCatch = pOnSide.Catch(&StepCatchSkipped, ASYNC_LOC);  // ④ 被跳过（仍在链上）
    lines.nFinally = __LINE__ + 1;
    CPromise<CTraceCtx> pFinally = pCatch.Finally(&StepFinally, ASYNC_LOC);  // ⑤ 收尾
    lines.nBridge = __LINE__ + 1;
    CPromise<CTraceCtx> pBridge = pFinally.ThenPromise(fnInnerChain, ASYNC_LOC);  // ⑥ 内层链（等它）
    lines.nBase = __LINE__ + 1;
    CPromise<CTraceCtx> pBase = pBridge.Then(&StepForkBase, ASYNC_LOC);  // ⑦ 分叉基座
    lines.nBranchA = __LINE__ + 1;
    CPromise<CTraceCtx> pBranchA = pBase.Then(&StepBranchA, ASYNC_LOC);  // ⑧ 分支 A
    lines.nBranchB = __LINE__ + 1;
    CPromise<CTraceCtx> pBranchB = pBase.Then(&StepBranchB, ASYNC_LOC);  // ⑨ 分支 B（最深）

    ASSERT_TRUE(pBranchA.Await().IsFulfilled());
    ASSERT_TRUE(pBranchB.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 1);             // 主链跑过（只有链根累加）
    ASSERT_TRUE(spCtx->bFinallyRan.load());  // finally 层执行了
    ASSERT_TRUE(!spCtx->bCatchRan.load());   // Catch 层被跳过（但它在链上，见下面的断言）

    //---------------- 通知：落定前登记 → 就地（复用触发层的帧） ----------------

    std::shared_ptr<CTraceCtx> spGateCtx = std::make_shared<CTraceCtx>();
    const int nLineGated = __LINE__ + 1;
    CPromise<CTraceCtx> pGated = execMain.NewPromise(spGateCtx, &StepGated, ASYNC_LOC);
    pGated.OnSettled(
        [spGateCtx](CPromiseResult)
        {
#if defined(ASYNC_DEBUG_TRACE)
            CaptureNow(spGateCtx->capNoticeInline);
#endif
            spGateCtx->bNoticeDone.store(true);
        });
    spGateCtx->bGateOpened.store(true);  // 放行：这一层这才落定（通知必定已登记）
    ASSERT_TRUE(pGated.Await().IsFulfilled());
    ASSERT_TRUE(WaitFlag(spGateCtx->bNoticeDone));

    //---------------- 通知：落定之后才登记 → 投递（不在任何层里） ----------------

    pGated.OnSettled(
        [spGateCtx](CPromiseResult)
        {
#if defined(ASYNC_DEBUG_TRACE)
            CaptureNow(spGateCtx->capNoticePosted);
#endif
            spGateCtx->bPostedDone.store(true);
        });
    ASSERT_TRUE(WaitFlag(spGateCtx->bPostedDone));

    //---------------- 协程：CO_AWAIT 等自己起的子链 ----------------

    std::shared_ptr<CProbeCoroutine> pCoro = execMain.CoStart<CProbeCoroutine>(spCtx);
    ASSERT_TRUE(pCoro->Await().IsFulfilled());

    //---------------- 异常路径：层内抛异常 → kException，帧栈照样弹回 ----------------

    lines.nThrow = __LINE__ + 1;
    CPromise<CTraceCtx> pThrow = execMain.NewPromise(spCtx, &StepCaptureThenThrow, ASYNC_LOC);
    const CPromiseResult rThrow = pThrow.Catch(&StepPassThrough, ASYNC_LOC).Await();
    ASSERT_TRUE(rThrow.IsRejected());
    ASSERT_TRUE(rThrow.Code() == static_cast<int>(common::async::kException));

#if defined(ASYNC_DEBUG_TRACE)

    //================ 主链最深（分支 B）看到的是整条链：8 层，深度 0…7 ================
    const CExpect vecExpectMain[8] = {
        {"then", lines.nBranchB},     // #0 本层（分支 B）
        {"then", lines.nBase},        // #1 分叉基座
        {"then", lines.nBridge},      // #2 ThenPromise（内层链挂在主链上的那一层）
        {"finally", lines.nFinally},  // #3
        {"catch", lines.nCatch},      // #4 被跳过的 Catch：照样在链上
        {"then", lines.nOther},       // #5 跑在另一个执行器上的层
        {"then", lines.nInline},      // #6 ThenInline
        {"then", lines.nRoot},        // #7 链根（到这里再往上没有了）
    };
    AssertChain(spCtx->capDeepest, vecExpectMain, 8);

    // 一行描述覆盖同一条链（#0 … #7，7 个箭头）。
    ASSERT_TRUE(spCtx->capDeepest.strChain.find("test_async_trace.cpp") != std::string::npos);
    ASSERT_TRUE(spCtx->capDeepest.strChain.find("#0 then") == 0);
    ASSERT_TRUE(spCtx->capDeepest.strChain.find("#7 then") != std::string::npos);
    ASSERT_EQ(CountArrows(spCtx->capDeepest.strChain), 7);

    // 分支 A：同样是「自己 + 整条主链」，但**看不到兄弟分支**（只往上游走）。
    const CExpect vecExpectBranchA[8] = {
        {"then", lines.nBranchA},
        {"then", lines.nBase},
        {"then", lines.nBridge},
        {"finally", lines.nFinally},
        {"catch", lines.nCatch},
        {"then", lines.nOther},
        {"then", lines.nInline},
        {"then", lines.nRoot},
    };
    AssertChain(spCtx->capBranchA, vecExpectBranchA, 8);
    ASSERT_TRUE(!HasLine(spCtx->capBranchA, lines.nBranchB));
    ASSERT_TRUE(!HasLine(spCtx->capDeepest, lines.nBranchA));

    // 中间各层：链就是「本层 + 上游」，层数 = 它在链上的位置 + 1。
    const CExpect vecExpectRoot[1] = {{"then", lines.nRoot}};
    AssertChain(spCtx->capRoot, vecExpectRoot, 1);

    const CExpect vecExpectInline[2] = {{"then", lines.nInline}, {"then", lines.nRoot}};
    AssertChain(spCtx->capInline, vecExpectInline, 2);

    // 换执行器 / 换线程不影响链：第 3 层照样看得到「自己 + 前两层」。
    const CExpect vecExpectOther[3] = {{"then", lines.nOther}, {"then", lines.nInline}, {"then", lines.nRoot}};
    AssertChain(spCtx->capOtherExec, vecExpectOther, 3);

    //================ 子链 → 父链：内层链的链根挂在「起它的那一层」下面 ================
    // 内层链是在 ⑥（ThenPromise 层）的工厂里现搭的 → 它的链根挂到 ⑥ 上，于是从内层最深一层
    // 就能一路追回主链链根（内层两段 + 主链前缀 = 8 层）。
    const CExpect vecExpectInnerSecond[8] = {
        {"then", spCtx->nLineInnerSecond},
        {"then", spCtx->nLineInnerFirst},
        {"then", lines.nBridge},
        {"finally", lines.nFinally},
        {"catch", lines.nCatch},
        {"then", lines.nOther},
        {"then", lines.nInline},
        {"then", lines.nRoot},
    };
    AssertChain(spCtx->capInnerSecond, vecExpectInnerSecond, 8);

    const CExpect vecExpectInnerFirst[7] = {
        {"then", spCtx->nLineInnerFirst},
        {"then", lines.nBridge},
        {"finally", lines.nFinally},
        {"catch", lines.nCatch},
        {"then", lines.nOther},
        {"then", lines.nInline},
        {"then", lines.nRoot},
    };
    AssertChain(spCtx->capInnerFirst, vecExpectInnerFirst, 7);

    // 反过来：子链在父链的**下游**，所以主链上任何一层都看不到它（只往上游走）。
    ASSERT_TRUE(!HasLine(spCtx->capDeepest, spCtx->nLineInnerFirst));

    //================ 通知：就地看得到「触发它的那一层」，投递看不到层 ================
    const CExpect vecExpectNoticeInline[1] = {{"then", nLineGated}};
    AssertChain(spGateCtx->capNoticeInline, vecExpectNoticeInline, 1);

    // 投递送达：通知不是层，没有自己的帧 → 不在任何层里。
    ASSERT_TRUE(!spGateCtx->capNoticePosted.bVisited);
    ASSERT_TRUE(!spGateCtx->capNoticePosted.bHasCurrent);
    ASSERT_TRUE(spGateCtx->capNoticePosted.vecChain.empty());

    //================ 协程：await 的是自己起的子链；恢复点两种都合法 ================
    const CExpect vecExpectCoroStep[1] = {{"then", spCtx->nLineCoroStep}};
    AssertChain(spCtx->capCoroStep, vecExpectCoroStep, 1);

    // 恢复点：就地续跑 → 落在「被 await 的那一层」的帧里；投递续跑（线程池有积压时）
    // → 不在任何层里。两种都不丢链，只是调度选择不同，所以只断言这个上界。
    ASSERT_TRUE(!spCtx->capCoroAfter.bHasCurrent ||
                (spCtx->capCoroAfter.vecChain.size() == 1 && spCtx->capCoroAfter.vecChain[0].loc.nLine == spCtx->nLineCoroStep));

    //================ 异常路径：抛之前链是完整的，抛之后帧栈干净 ================
    const CExpect vecExpectThrowing[1] = {{"then", lines.nThrow}};
    AssertChain(spCtx->capThrowing, vecExpectThrowing, 1);

    // 异常路径后当前线程的帧栈必须已弹空（否则后面的遍历会看到残留的层）。
    ASSERT_TRUE(common::async::CurrentLayer() == NULL);
    ASSERT_TRUE(common::async::DescribeLayerChain().empty());

#else

    // 发布构建：trace 整段不存在（接口都没有），这里只确认「流程本身」跑对了 ——
    // 采集 / 链断言这两组东西在发布构建里根本不参与编译。
    (void)lines;
    (void)nLineGated;
    ASSERT_EQ(spCtx->nValue, 1);                 // 主链跑过
    ASSERT_TRUE(spCtx->bFinallyRan.load());      // finally 层执行了
    ASSERT_TRUE(spGateCtx->bNoticeDone.load());  // 通知送达
    ASSERT_TRUE(spGateCtx->bPostedDone.load());

#endif

    execMain.Stop();
    execSide.Stop();
}

#if defined(ASYNC_DEBUG_TRACE)

/// @brief 层外调用：不是层 → 拿不到当前层，遍历返回 false（不崩）。
///
/// 整个用例都是「层外契约」（调的全是 trace 接口），所以只在调试构建存在。
TEST(Trace_NotInsideLayer)
{
    ASSERT_TRUE(common::async::CurrentLayer() == NULL);
    ASSERT_TRUE(!common::async::VisitLayerChain(
        [](const CLayerInfo&)
        {
        }));

    CAsyncExecutor exec(1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTraceCtx> spCtx = std::make_shared<CTraceCtx>();
    spCtx->nLineStart = __LINE__ + 1;
    ASSERT_TRUE(exec.NewPromise(spCtx, &StepRoot, ASYNC_LOC).Await().IsFulfilled());

    // 层外起的链没有「父层」—— 链根就是链根（它的上游要等有人 adopt / 或它在层里起链时才挂上）。
    const CExpect vecExpectAlone[1] = {{"then", spCtx->nLineStart}};
    AssertChain(spCtx->capRoot, vecExpectAlone, 1);

    // 层跑在 worker 线程上；主线程（调用方）始终不在层里。
    ASSERT_TRUE(common::async::CurrentLayer() == NULL);
    ASSERT_TRUE(common::async::DescribeLayerChain().empty());
    exec.Stop();
}

#endif  // defined(ASYNC_DEBUG_TRACE)
