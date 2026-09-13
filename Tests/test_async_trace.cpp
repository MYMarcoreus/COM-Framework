/// @file test_async_trace.cpp
/// 异步调用链（trace）：验证「在异步层里能不能看到**完整**的调用链」。
///
/// 主用例 `Trace_CompleteChainInComplexFlow` 是一条把各种层形态混在一起的主链：
/// ① 具名 then（链根）→ ② 具名 then（本链线程上就地级联）→ ③ 具名 then
/// → ④ 被跳过的 `Catch` → ⑤ `Finally` → ⑥ `ThenPromise` 内层链
/// → ⑦ 分叉基座 → ⑧/⑨ 两支；然后在**最深的地方**把整条链逐层断言出来
/// （层数、模式、注册点行号、深度、当前层标记、**跑在哪台执行器上**、一行描述），
/// 另外把特殊位置逐个钉住：
///   - 子链 → 父链：内层链（`ThenPromise`）的链根挂在**起它的那一层**下面 → 从内层里能一路
///     追回主链；反过来主链看不到子链（只往上游走）；层外起的链没有父层；
///   - 跨执行器：内层链跑在**另一个执行器**上（自己的 worker），链本身不受影响 ——
///     链上的层都在本链执行器线程上，跨执行器就跨链（也直接看执行器名那一列）；
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

#include <algorithm>
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
    std::string strChain;              ///< `DescribeLayerChain()` 的结果（一行压缩链）。
    std::string strDumpBlock;          ///< `DescribeLayerChainBlock()` 的结果（排障用多行块）。

    CCapture() : bHasCurrent(false), infoCurrent(), bVisited(false), vecChain(), strChain(), strDumpBlock()
    {}
};

#endif  // defined(ASYNC_DEBUG_TRACE)

/// @brief 期望的链：逐层的（模式, 注册点行号, 执行器名）。
struct CExpect
{
    const char* pszMode;  ///< then / catch / finally。
    int nLine;            ///< 注册点行号（`__LINE__ + 1` 采集）。

    /// 本层跑在哪个执行器上：`NULL` = 不检查；`"-"` = 期望「没有执行器」
    /// （这一层没跑过 handler，比如被跳过的 catch / 桥接层）；否则执行器名。
    const char* pszExec;
};

/// @brief 主链各层的注册点（每个 `__LINE__ + 1` 紧跟一次挂层）。
///
/// 放在一个结构里是为了发布构建下只需一句 `(void)lines;` —— trace 关掉时它们没有用处。
struct CLines
{
    int nRoot;     ///< `exec.NewPromise`（链根）
    int nSecond;   ///< 第二个 then（就地级联）
    int nThird;    ///< 第三个 then（跨执行器回来后仍在本链执行器上）
    int nCatch;    ///< `Catch`（本流程被跳过，但仍在链上）
    int nFinally;  ///< `Finally`
    int nBridge;   ///< `ThenPromise`（内层链挂在主链上的那一层）
    int nBase;     ///< 分叉基座
    int nBranchA;  ///< 分叉分支 A
    int nBranchB;  ///< 分叉分支 B
    int nThrow;    ///< 异常路径那条小链的首层

    CLines() : nRoot(0), nSecond(0), nThird(0), nCatch(0), nFinally(0), nBridge(0), nBase(0), nBranchA(0), nBranchB(0), nThrow(0)
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
    CCapture capSecond;        ///< 第二个 then 层
    CCapture capThird;         ///< 第三个 then 层
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
          capSecond(),
          capThird(),
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
    cap.strDumpBlock = common::async::DescribeLayerChainBlock();
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
/// （只有第 0 项为真）/ 跑在哪台执行器上（`pszExec` 非空才查）；
/// 最后再确认 `CurrentLayer()` 与链首是同一层。
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
        if (pExpect[i].pszExec != NULL)
        {
            if (pExpect[i].pszExec[0] == '-')
            {
                ASSERT_TRUE(info.spExecName == NULL);  // 没跑过 handler → 没有执行器
            }
            else
            {
                ASSERT_TRUE(info.spExecName != NULL);
                ASSERT_TRUE(*info.spExecName == std::string(pExpect[i].pszExec));  // 这一层真跑在那台执行器上
            }
        }
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

/// @brief 文本行数（`DescribeLayerChainBlock()` 的块：头行 + 每层一行）。
int CountLines(const std::string& strText)
{
    int nCount = 0;
    for (size_t i = 0; i < strText.size(); ++i)
    {
        if (strText[i] == '\n')
        {
            ++nCount;
        }
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

/// 层：第二个 then（默认亲和 —— 已在本链执行器线程上，所以**就地**级联）。
CPromiseResult StepSecond(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    TRACE_CAPTURE(capSecond);
    return CPromiseResult::Resolve();
}

/// 层：第三个 then（默认亲和 —— 跨执行器回来也照样落在本链执行器线程上）。
CPromiseResult StepThird(CPromiseResult upResult, const std::shared_ptr<CTraceCtx>& spCtx)
{
    (void)upResult;
    TRACE_CAPTURE(capThird);
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
    CAsyncExecutor execMain("trace-main", 2);  // 主链：2 线程，分叉两支正好一支就地、一支投递
    CAsyncExecutor execSide("trace-side", 1);  // 内层链的执行器（链上唯一换执行器的地方）
    ASSERT_TRUE(execMain.Start());
    ASSERT_TRUE(execSide.Start());

    std::shared_ptr<CTraceCtx> spCtx = std::make_shared<CTraceCtx>();
    CLines lines;

    // 内层链的工厂（单独具名：这样 `ThenPromise` 那一行能整行写下，注册点行号好断言）。
    CPromise<CTraceCtx>::PromiseFactory fnInnerChain = [&execSide, spCtx](const std::shared_ptr<CTraceCtx>& spInnerCtx)
    {
        TRACE_LINE(spCtx->nLineInnerFirst = __LINE__ + 1);
        CPromise<CTraceCtx> pInner = execSide.NewPromise(spInnerCtx, &StepInnerFirst, ASYNC_LOC);
        TRACE_LINE(spCtx->nLineInnerSecond = __LINE__ + 1);
        return pInner.Then(&StepInnerSecond, ASYNC_LOC);
    };

    //---------------- 主链：一条「下单」流程，把层形态混起来 ----------------

    lines.nRoot = __LINE__ + 1;
    CPromise<CTraceCtx> pRoot = execMain.NewPromise(spCtx, &StepRoot, ASYNC_LOC);  // ① 链根
    lines.nSecond = __LINE__ + 1;
    CPromise<CTraceCtx> pSecond = pRoot.Then(&StepSecond, ASYNC_LOC);  // ② then（本链线程上就地级联）
    lines.nThird = __LINE__ + 1;
    CPromise<CTraceCtx> pThird = pSecond.Then(&StepThird, ASYNC_LOC);  // ③ then（同一执行器）
    lines.nCatch = __LINE__ + 1;
    CPromise<CTraceCtx> pCatch = pThird.Catch(&StepCatchSkipped, ASYNC_LOC);  // ④ 被跳过（仍在链上）
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
        {"then", lines.nBranchB, "trace-main"},     // #0 本层（分支 B）
        {"then", lines.nBase, "trace-main"},        // #1 分叉基座
        {"then", lines.nBridge, "-"},               // #2 ThenPromise（内层链挂在主链上的那一层）
        {"finally", lines.nFinally, "trace-main"},  // #3
        {"catch", lines.nCatch, "-"},               // #4 被跳过的 Catch：照样在链上
        {"then", lines.nThird, "trace-main"},       // #5 第三个 then（跨执行器回来仍在本链线程上）
        {"then", lines.nSecond, "trace-main"},      // #6 第二个 then（本链线程上就地级联）
        {"then", lines.nRoot, "trace-main"},        // #7 链根（到这里再往上没有了）
    };
    AssertChain(spCtx->capDeepest, vecExpectMain, 8);

    // 一行描述覆盖同一条链（#0 … #7，7 个箭头）。
    ASSERT_TRUE(spCtx->capDeepest.strChain.find("test_async_trace.cpp") != std::string::npos);
    ASSERT_TRUE(spCtx->capDeepest.strChain.find("#0 then") == 0);
    ASSERT_TRUE(spCtx->capDeepest.strChain.find("#7 then") != std::string::npos);
    ASSERT_EQ(CountArrows(spCtx->capDeepest.strChain), 7);

    // 排障用的多行块（dump 接口）：头行给总数，一层一行，当前层带标记；8 层不到上限 → 不省略。
    const std::string& strDump = spCtx->capDeepest.strDumpBlock;
    ASSERT_TRUE(strDump.find("[async 链] 共 8 层") == 0);  // 头行先给总数
    ASSERT_EQ(CountLines(strDump), 9);                     // 头行 + 8 层
    ASSERT_TRUE(strDump.find("← 当前层") != std::string::npos);
    ASSERT_TRUE(strDump.find("省略中间") == std::string::npos);
    ASSERT_TRUE(strDump.find("[trace-main]") != std::string::npos);  // 带执行器列

    // 分支 A：同样是「自己 + 整条主链」，但**看不到兄弟分支**（只往上游走）。
    const CExpect vecExpectBranchA[8] = {
        {"then", lines.nBranchA, "trace-main"},
        {"then", lines.nBase, "trace-main"},
        {"then", lines.nBridge, "-"},
        {"finally", lines.nFinally, "trace-main"},
        {"catch", lines.nCatch, "-"},
        {"then", lines.nThird, "trace-main"},
        {"then", lines.nSecond, "trace-main"},
        {"then", lines.nRoot, "trace-main"},
    };
    AssertChain(spCtx->capBranchA, vecExpectBranchA, 8);
    ASSERT_TRUE(!HasLine(spCtx->capBranchA, lines.nBranchB));
    ASSERT_TRUE(!HasLine(spCtx->capDeepest, lines.nBranchA));

    // 中间各层：链就是「本层 + 上游」，层数 = 它在链上的位置 + 1。
    const CExpect vecExpectRoot[1] = {{"then", lines.nRoot, "trace-main"}};
    AssertChain(spCtx->capRoot, vecExpectRoot, 1);

    const CExpect vecExpectSecond[2] = {{"then", lines.nSecond, "trace-main"}, {"then", lines.nRoot, "trace-main"}};
    AssertChain(spCtx->capSecond, vecExpectSecond, 2);

    // 同一执行器内：第三层照样看得到「自己 + 前两层」。
    const CExpect vecExpectThird[3] = {
        {"then", lines.nThird, "trace-main"}, {"then", lines.nSecond, "trace-main"}, {"then", lines.nRoot, "trace-main"}};
    AssertChain(spCtx->capThird, vecExpectThird, 3);

    //================ 子链 → 父链：内层链的链根挂在「起它的那一层」下面 ================
    // 内层链是在 ⑥（ThenPromise 层）的工厂里现搭的 → 它的链根挂到 ⑥ 上，于是从内层最深一层
    // 就能一路追回主链链根（内层两段 + 主链前缀 = 8 层）。
    const CExpect vecExpectInnerSecond[8] = {
        {"then", spCtx->nLineInnerSecond, "trace-side"},
        {"then", spCtx->nLineInnerFirst, "trace-side"},
        {"then", lines.nBridge, "-"},
        {"finally", lines.nFinally, "trace-main"},
        {"catch", lines.nCatch, "-"},
        {"then", lines.nThird, "trace-main"},
        {"then", lines.nSecond, "trace-main"},
        {"then", lines.nRoot, "trace-main"},
    };
    AssertChain(spCtx->capInnerSecond, vecExpectInnerSecond, 8);

    const CExpect vecExpectInnerFirst[7] = {
        {"then", spCtx->nLineInnerFirst, "trace-side"},
        {"then", lines.nBridge, "-"},
        {"finally", lines.nFinally, "trace-main"},
        {"catch", lines.nCatch, "-"},
        {"then", lines.nThird, "trace-main"},
        {"then", lines.nSecond, "trace-main"},
        {"then", lines.nRoot, "trace-main"},
    };
    AssertChain(spCtx->capInnerFirst, vecExpectInnerFirst, 7);

    // 反过来：子链在父链的**下游**，所以主链上任何一层都看不到它（只往上游走）。
    ASSERT_TRUE(!HasLine(spCtx->capDeepest, spCtx->nLineInnerFirst));

    //================ 跨执行器：内层链在 execSide 的线程上，链本身不受影响 ================
    // （「层都在本链执行器线程上」—— 跨执行器的是**另一条链**：内层链有自己的执行器。）
    ASSERT_TRUE(spCtx->capInnerSecond.vecChain[0].tid == spCtx->capInnerSecond.vecChain[1].tid);  // 内层两层同一条 worker
    ASSERT_TRUE(spCtx->capInnerSecond.vecChain[0].tid != spCtx->capInnerSecond.vecChain[3].tid);  // 与父链线程不同

    //================ 通知：就地看得到「触发它的那一层」，投递看不到层 ================
    const CExpect vecExpectNoticeInline[1] = {{"then", nLineGated, "trace-main"}};
    AssertChain(spGateCtx->capNoticeInline, vecExpectNoticeInline, 1);

    // 投递送达：通知不是层，没有自己的帧 → 不在任何层里。
    ASSERT_TRUE(!spGateCtx->capNoticePosted.bVisited);
    ASSERT_TRUE(!spGateCtx->capNoticePosted.bHasCurrent);
    ASSERT_TRUE(spGateCtx->capNoticePosted.vecChain.empty());

    //================ 协程：await 的是自己起的子链；恢复点两种都合法 ================
    const CExpect vecExpectCoroStep[1] = {{"then", spCtx->nLineCoroStep, "trace-main"}};
    AssertChain(spCtx->capCoroStep, vecExpectCoroStep, 1);

    // 恢复点：就地续跑 → 落在「被 await 的那一层」的帧里；投递续跑（线程池有积压时）
    // → 不在任何层里。两种都不丢链，只是调度选择不同，所以只断言这个上界。
    ASSERT_TRUE(!spCtx->capCoroAfter.bHasCurrent ||
                (spCtx->capCoroAfter.vecChain.size() == 1 && spCtx->capCoroAfter.vecChain[0].loc.nLine == spCtx->nLineCoroStep));

    //================ 异常路径：抛之前链是完整的，抛之后帧栈干净 ================
    const CExpect vecExpectThrowing[1] = {{"then", lines.nThrow, "trace-main"}};
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

    CAsyncExecutor exec("trace-main", 1);
    ASSERT_TRUE(exec.Start());

    std::shared_ptr<CTraceCtx> spCtx = std::make_shared<CTraceCtx>();
    spCtx->nLineStart = __LINE__ + 1;
    ASSERT_TRUE(exec.NewPromise(spCtx, &StepRoot, ASYNC_LOC).Await().IsFulfilled());

    // 层外起的链没有「父层」—— 链根就是链根（它的上游要等有人 adopt / 或它在层里起链时才挂上）。
    const CExpect vecExpectAlone[1] = {{"then", spCtx->nLineStart, "trace-main"}};
    AssertChain(spCtx->capRoot, vecExpectAlone, 1);

    // 层跑在 worker 线程上；主线程（调用方）始终不在层里。
    ASSERT_TRUE(common::async::CurrentLayer() == NULL);
    ASSERT_TRUE(common::async::DescribeLayerChain().empty());
    ASSERT_TRUE(common::async::DescribeLayerChainBlock().empty());   // 层外没链可 dump
    ASSERT_TRUE(common::async::DescribeLayerChainBlock(0).empty());  // 不限层数也一样
    common::async::DumpLayerChain();                                 // 只该打一行提示（不崩）
    exec.Stop();
}

// ====================================================================
// 更复杂的场景（一）：多层子链 —— 子链里再起子链
//
// 形状（3 条链：主链 → sub1 → sub2）：
//
//   主链：root → pre → await1(ThenPromise)              链#A
//                        └─ sub1：root → await2(ThenPromise)   链#B（挂在 await1 下面）
//                                                └─ sub2：root → deep   链#C（挂在 await2 下面）
//
// 在 sub2 最深一层采集：应当看到**跨 3 条链**的完整祖先路径，且链号分成 3 段、
// 段边界正好落在两条子链的链根上。
// ====================================================================

/// @brief 多层子链用例的上下文。
struct CNestCtx
{
    int nValue;         ///< 累加（确认链真的跑过）。
    int nLineRoot;      ///< 主链链根。
    int nLinePre;       ///< 主链前缀层。
    int nLineAwait1;    ///< 主链上等 sub1 的那一层（`ThenPromise` #1）。
    int nLineSub1Root;  ///< sub1 链根。
    int nLineAwait2;    ///< sub1 上等 sub2 的那一层（`ThenPromise` #2）。
    int nLineSub2Root;  ///< sub2 链根。
    int nLineSub2Deep;  ///< sub2 最深（采集点）。

    CCapture capDeep;  ///< sub2 最深一层的采集。

    CNestCtx()
        : nValue(0),
          nLineRoot(0),
          nLinePre(0),
          nLineAwait1(0),
          nLineSub1Root(0),
          nLineAwait2(0),
          nLineSub2Root(0),
          nLineSub2Deep(0),
          capDeep()
    {}
};

/// @brief 主链链根（累加一下，确认链真跑过）。
CPromiseResult NestStepRoot(CPromiseResult upResult, const std::shared_ptr<CNestCtx>& spCtx)
{
    (void)upResult;
    ++spCtx->nValue;
    return CPromiseResult::Resolve();
}

/// @brief 什么都不做的层（只为了让链成形）。
CPromiseResult NestStepNoop(CPromiseResult upResult, const std::shared_ptr<CNestCtx>& spCtx)
{
    (void)upResult;
    (void)spCtx;
    return CPromiseResult::Resolve();
}

/// @brief sub2 最深一层：采集「跨 3 条链」的祖先路径。
CPromiseResult NestStepSub2Deep(CPromiseResult upResult, const std::shared_ptr<CNestCtx>& spCtx)
{
    (void)upResult;
    CaptureNow(spCtx->capDeep);
    return CPromiseResult::Resolve();
}

/// @brief 链上「链号分段」的段数（连续相同的链号算一段）。
///
/// 子链挂在父链的某一层下面，所以祖先路径上链号一定是「一段一段」的，段数 = 跨了几条链。
int CountChainSegments(const std::vector<CLayerInfo>& vecChain)
{
    int nSegments = 0;
    for (size_t i = 0; i < vecChain.size(); ++i)
    {
        if (i == 0 || vecChain[i].nChainId != vecChain[i - 1].nChainId)
        {
            ++nSegments;
        }
    }
    return nSegments;
}

/// @brief 这些层号两两不同（层号全局唯一）。
bool AreLayerIdsUnique(const std::vector<unsigned>& vecIds)
{
    std::vector<unsigned> vecSorted = vecIds;
    std::sort(vecSorted.begin(), vecSorted.end());
    return std::adjacent_find(vecSorted.begin(), vecSorted.end()) == vecSorted.end();
}

TEST(Trace_NestedSubChainsThreeLevels)
{
    CAsyncExecutor exec("trace-main", 2);
    ASSERT_TRUE(exec.Start());
    const std::shared_ptr<CNestCtx> spCtx = std::make_shared<CNestCtx>();

    // sub2 的工厂：链根 + 一层（这一层负责采集）。
    const CPromise<CNestCtx>::PromiseFactory fnSub2 = [&exec, spCtx](const std::shared_ptr<CNestCtx>& spSelf)
    {
        spCtx->nLineSub2Root = __LINE__ + 1;
        CPromise<CNestCtx> pSub2 = exec.NewPromise(spSelf, &NestStepNoop, ASYNC_LOC);
        spCtx->nLineSub2Deep = __LINE__ + 1;
        return pSub2.Then(&NestStepSub2Deep, ASYNC_LOC);
    };

    // sub1 的工厂：链根 + 一层（这一层里去起 sub2 —— 子链套子链）。
    const CPromise<CNestCtx>::PromiseFactory fnSub1 = [&exec, spCtx, fnSub2](const std::shared_ptr<CNestCtx>& spSelf)
    {
        spCtx->nLineSub1Root = __LINE__ + 1;
        CPromise<CNestCtx> pSub1 = exec.NewPromise(spSelf, &NestStepNoop, ASYNC_LOC);
        spCtx->nLineAwait2 = __LINE__ + 1;
        return pSub1.ThenPromise(fnSub2, ASYNC_LOC);
    };

    // 主链：root → pre → 等 sub1。
    spCtx->nLineRoot = __LINE__ + 1;
    CPromise<CNestCtx> pRoot = exec.NewPromise(spCtx, &NestStepRoot, ASYNC_LOC);
    spCtx->nLinePre = __LINE__ + 1;
    CPromise<CNestCtx> pPre = pRoot.Then(&NestStepNoop, ASYNC_LOC);
    spCtx->nLineAwait1 = __LINE__ + 1;
    CPromise<CNestCtx> pTail = pPre.ThenPromise(fnSub1, ASYNC_LOC);

    ASSERT_TRUE(pTail.Await().IsFulfilled());
    ASSERT_EQ(spCtx->nValue, 1);
    ASSERT_TRUE(spCtx->capDeep.bVisited);

    // 祖先路径（近 → 远）：sub2 深层(0) → sub2 链根(1) → sub1 上等 sub2 的那层(2) → sub1 链根(3)
    //                   → 主链上等 sub1 的那层(4) → 主链前缀(5) → 主链链根(6)
    const CExpect vecExpect[7] = {
        {"then", spCtx->nLineSub2Deep, "trace-main"},
        {"then", spCtx->nLineSub2Root, "trace-main"},
        {"then", spCtx->nLineAwait2, "-"},
        {"then", spCtx->nLineSub1Root, "trace-main"},
        {"then", spCtx->nLineAwait1, "-"},
        {"then", spCtx->nLinePre, "trace-main"},
        {"then", spCtx->nLineRoot, "trace-main"},
    };
    AssertChain(spCtx->capDeep, vecExpect, 7);

    const std::vector<CLayerInfo>& vec = spCtx->capDeep.vecChain;

    // ---- 链号分 3 段，边界正好落在两条子链的链根上 ----
    ASSERT_EQ(CountChainSegments(vec), 3);
    ASSERT_TRUE(vec[1].bChainRoot && vec[1].bSubChain);   // sub2 的链根（挂在 sub1 里）
    ASSERT_TRUE(vec[3].bChainRoot && vec[3].bSubChain);   // sub1 的链根（挂在主链里）
    ASSERT_TRUE(vec[6].bChainRoot && !vec[6].bSubChain);  // 主链链根（最外层）
    ASSERT_TRUE(!vec[0].bChainRoot && !vec[2].bChainRoot && !vec[4].bChainRoot && !vec[5].bChainRoot);
    ASSERT_EQ(vec[0].nChainId, vec[1].nChainId);  // sub2 自己是一段
    ASSERT_EQ(vec[2].nChainId, vec[3].nChainId);  // sub1 是一段
    for (size_t i = 4; i < vec.size(); ++i)
    {
        ASSERT_EQ(vec[i].nChainId, vec[6].nChainId);  // 主链是一段
    }
    // 链号在链根创建时分配，子链总是后建的 → 子链链号 > 父链链号。
    ASSERT_TRUE(vec[0].nChainId > vec[2].nChainId);
    ASSERT_TRUE(vec[2].nChainId > vec[4].nChainId);

    // ---- 当前层按定义还没落定；**已经跑过**的那些层都已落定并兑现 ----
    //
    // 要特别注意的是子链那两个「等子链」的层（vec[2] 在 sub1 上、vec[4] 在主链上）：
    // 子链（sub2 / sub1）还没落定，它们自然也还没落定 —— 这正是 `ThenPromise` 的语义，
    // 从子链内部看过去，这条链上一眼就能看出「谁还在等我」。
    ASSERT_TRUE(!vec[0].bSettled);                      // 本层正在跑
    ASSERT_TRUE(vec[1].bSettled && vec[1].bFulfilled);  // sub2 链根：跑过了
    ASSERT_TRUE(!vec[2].bSettled);                      // sub1 上等 sub2 的那一层：还在等
    ASSERT_TRUE(!vec[2].bFulfilled);
    ASSERT_TRUE(vec[3].bSettled && vec[3].bFulfilled);  // sub1 链根：跑过了
    ASSERT_TRUE(!vec[4].bSettled);                      // 主链上等 sub1 的那一层：还在等
    ASSERT_TRUE(!vec[4].bFulfilled);
    ASSERT_TRUE(vec[5].bSettled && vec[5].bFulfilled);  // 主链前缀
    ASSERT_TRUE(vec[6].bSettled && vec[6].bFulfilled);  // 主链链根

    // ---- 年龄 ≥ 本层耗时 ≥ 0（本层耗时是「创建之后到落定」里的一段） ----
    for (size_t i = 0; i < vec.size(); ++i)
    {
        ASSERT_TRUE(vec[i].nAgeMs >= 0);
        if (vec[i].bSettled)
        {
            ASSERT_TRUE(vec[i].nSelfMs >= 0);
            ASSERT_TRUE(vec[i].nAgeMs >= vec[i].nSelfMs);
        }
    }

    // ---- 层号全局唯一且非 0（同一层不会在链上出现两次） ----
    std::vector<unsigned> vecIds;
    for (size_t i = 0; i < vec.size(); ++i)
    {
        ASSERT_TRUE(vec[i].nLayerId != 0);
        vecIds.push_back(vec[i].nLayerId);
    }
    ASSERT_TRUE(AreLayerIdsUnique(vecIds));

    // ---- 一行描述：6 个箭头（7 层） ----
    ASSERT_EQ(CountArrows(spCtx->capDeep.strChain), 6);

    exec.Stop();
}

// ====================================================================
// 更复杂的场景（二）：并发多链互不串
//
// 同时跑 4 条链，每条链的**前缀层数不同**（0/1/2/3）—— 这就是指纹：
// 采集到的层数必须等于「本层 1 + 子链根 1 + 等子链层 1 + nExtra + 链根 1」。
// 串链了、或者上游指针串了，层数/行号立刻就对不上。
// ====================================================================

/// @brief 并发隔离用例的上下文（一条链一个）。
struct CMixCtx
{
    int nExtra;        ///< 这条链的前缀层数（每条链不同）。
    int nLineRoot;     ///< 链根。
    int nLineAwait;    ///< 等子链的那一层。
    int nLineSubRoot;  ///< 子链链根。
    int nLineSubDeep;  ///< 子链最深（采集点）。

    std::atomic<bool> bOk;  ///< 链跑完了（**断言只能在主测试线程做**，工作线程只记结果）。
    CCapture capDeep;       ///< 子链最深一层的采集。

    CMixCtx() : nExtra(0), nLineRoot(0), nLineAwait(0), nLineSubRoot(0), nLineSubDeep(0), bOk(false), capDeep()
    {}
};

/// @brief 子链最深一层：采集。
CPromiseResult MixStepSubDeep(CPromiseResult upResult, const std::shared_ptr<CMixCtx>& spCtx)
{
    (void)upResult;
    CaptureNow(spCtx->capDeep);
    return CPromiseResult::Resolve();
}

/// @brief 什么都不做的层。
CPromiseResult MixStepNoop(CPromiseResult upResult, const std::shared_ptr<CMixCtx>& spCtx)
{
    (void)upResult;
    (void)spCtx;
    return CPromiseResult::Resolve();
}

TEST(Trace_ConcurrentChainsDoNotMix)
{
    const int kChains = 4;
    CAsyncExecutor exec(kChains);
    ASSERT_TRUE(exec.Start());

    std::vector<std::shared_ptr<CMixCtx> > vecCtx;
    for (int i = 0; i < kChains; ++i)
    {
        const std::shared_ptr<CMixCtx> spCtx = std::make_shared<CMixCtx>();
        spCtx->nExtra = i;
        vecCtx.push_back(spCtx);
    }

    // 4 条链各从一个线程发起（在同一个执行器上并发跑）。
    std::vector<std::thread> vecThreads;
    for (int i = 0; i < kChains; ++i)
    {
        const std::shared_ptr<CMixCtx> spCtx = vecCtx[static_cast<size_t>(i)];
        vecThreads.push_back(std::thread(
            [&exec, spCtx]()
            {
                const CPromise<CMixCtx>::PromiseFactory fnSub = [&exec, spCtx](const std::shared_ptr<CMixCtx>& spSelf)
                {
                    spCtx->nLineSubRoot = __LINE__ + 1;
                    CPromise<CMixCtx> pSub = exec.NewPromise(spSelf, &MixStepNoop, ASYNC_LOC);
                    spCtx->nLineSubDeep = __LINE__ + 1;
                    return pSub.Then(&MixStepSubDeep, ASYNC_LOC);
                };

                spCtx->nLineRoot = __LINE__ + 1;
                CPromise<CMixCtx> p = exec.NewPromise(spCtx, &MixStepNoop, ASYNC_LOC);
                for (int k = 0; k < spCtx->nExtra; ++k)
                {
                    p = p.Then(&MixStepNoop, ASYNC_LOC);  // 前缀层：都注册在**同一行**
                }
                spCtx->nLineAwait = __LINE__ + 1;
                p = p.ThenPromise(fnSub, ASYNC_LOC);
                const bool bOk = p.Await().IsFulfilled();
                spCtx->bOk.store(bOk);  // 工作线程只记结果（断言会抛异常，只能在主线程用）
            }));
    }
    for (size_t i = 0; i < vecThreads.size(); ++i)
    {
        vecThreads[i].join();
    }

    std::vector<unsigned> vecAllIds;
    for (int i = 0; i < kChains; ++i)
    {
        const std::shared_ptr<CMixCtx> spCtx = vecCtx[static_cast<size_t>(i)];
        ASSERT_TRUE(spCtx->bOk.load());
        ASSERT_TRUE(spCtx->capDeep.bVisited);

        const std::vector<CLayerInfo>& vec = spCtx->capDeep.vecChain;

        // 指纹：层数 = 本层 1 + 子链根 1 + 等子链层 1 + nExtra + 链根 1。
        ASSERT_EQ(vec.size(), static_cast<size_t>(spCtx->nExtra + 4));
        ASSERT_EQ(vec[0].loc.nLine, spCtx->nLineSubDeep);
        ASSERT_EQ(vec[1].loc.nLine, spCtx->nLineSubRoot);
        ASSERT_EQ(vec[2].loc.nLine, spCtx->nLineAwait);
        ASSERT_EQ(vec[vec.size() - 1].loc.nLine, spCtx->nLineRoot);

        // 中间那些前缀层都注册在**同一行**：行号必须全相同，且不等于固定那几层。
        // （不写 `__LINE__ + N` 去硬碰：Allman 的 `{` 单独一行，偏移量一改格式就错。）
        for (size_t k = 3; k + 1 < vec.size(); ++k)
        {
            ASSERT_EQ(vec[k].loc.nLine, vec[3].loc.nLine);
            ASSERT_TRUE(vec[k].loc.nLine != vec[2].loc.nLine);
            ASSERT_TRUE(vec[k].loc.nLine != vec[vec.size() - 1].loc.nLine);
        }

        // 深度沿链严格递增（0,1,2…），且只有第 0 项是当前层。
        for (size_t k = 0; k < vec.size(); ++k)
        {
            ASSERT_EQ(vec[k].nDepth, static_cast<int>(k));
            ASSERT_TRUE(vec[k].bCurrent == (k == 0));
            vecAllIds.push_back(vec[k].nLayerId);
        }

        // 两段链号（子链 + 父链），边界在子链链根上。
        ASSERT_EQ(CountChainSegments(vec), 2);
        ASSERT_TRUE(vec[1].bChainRoot && vec[1].bSubChain);
        ASSERT_TRUE(vec[vec.size() - 1].bChainRoot && !vec[vec.size() - 1].bSubChain);
        ASSERT_TRUE(vec[0].nChainId != vec[2].nChainId);
    }

    // 4 条链采到的层号两两不同：说明每条链看到的都是**自己**那条（没串链），
    // 也说明上游指针没成环（成环会让同一层在链上出现两次）。
    ASSERT_EQ(vecAllIds.size(), static_cast<size_t>(kChains * 4 + kChains * (kChains - 1) / 2));
    ASSERT_TRUE(AreLayerIdsUnique(vecAllIds));

    exec.Stop();
}

// ====================================================================
// 更复杂的场景（三）：深链 + 帧栈没有残留
//
// 两个要害：
//  ① 长链（256 层）的深度/层号/链号必须逐项对得上（跨了多次「内联深度超限→投递」的边界）；
//  ② 帧栈是 thread_local 的，所以层跑完之后必须干净：在同一**唯一** worker 上再投一个探针，
//     它必须「不在任何层里」—— 否则就是帧没弹（正常返回、抛异常两条路径都要查）。
// ====================================================================

/// @brief 深链 / 帧栈残留用例的上下文。
struct CResidueCtx
{
    int nLineRoot;     ///< 链根。
    int nLineDeep;     ///< 最深一层（采集点）。
    CCapture capDeep;  ///< 最深一层的采集。

    std::string strDumpDefault;  ///< 最深一层看到的 dump 块（默认上限）。
    std::string strDumpAll;      ///< 同上，但 `DescribeLayerChainBlock(0)`（不限层数）。

    std::atomic<bool> bProbeDone;     ///< 探针跑完了。
    std::atomic<bool> bProbeInLayer;  ///< 探针里「看到层」了（应为 false）。

    CResidueCtx() : nLineRoot(0), nLineDeep(0), capDeep(), strDumpDefault(), strDumpAll(), bProbeDone(false), bProbeInLayer(false)
    {}
};

/// @brief 什么都不做的层。
CPromiseResult ResidueStepNoop(CPromiseResult upResult, const std::shared_ptr<CResidueCtx>& spCtx)
{
    (void)upResult;
    (void)spCtx;
    return CPromiseResult::Resolve();
}

/// @brief 最深一层：采集整条深链。
CPromiseResult ResidueStepDeep(CPromiseResult upResult, const std::shared_ptr<CResidueCtx>& spCtx)
{
    (void)upResult;
    CaptureNow(spCtx->capDeep);
    spCtx->strDumpDefault = common::async::DescribeLayerChainBlock();  // 默认上限
    spCtx->strDumpAll = common::async::DescribeLayerChainBlock(0);     // 不限层数
    return CPromiseResult::Resolve();
}

/// @brief 层内抛异常（查「异常路径也把帧弹出去了」）。
CPromiseResult ResidueStepThrow(CPromiseResult upResult, const std::shared_ptr<CResidueCtx>& spCtx)
{
    (void)upResult;
    (void)spCtx;
    throw std::runtime_error("trace：层内抛异常");
}

/// @brief 原样透传（catch 层用）。
CPromiseResult ResidueStepPassThrough(CPromiseResult upResult, const std::shared_ptr<CResidueCtx>& spCtx)
{
    (void)spCtx;
    return upResult;
}

/// @brief 在一个**唯一 worker** 上投一个探针：它必须不在任何层里（帧没残留）。
///
/// @param exec 单线程执行器（探针必然跑在刚刚跑过链的那条 worker 上）。
/// @param spCtx 上下文（探针结果写回它）。
void PostResidueProbe(CAsyncExecutor& exec, const std::shared_ptr<CResidueCtx>& spCtx)
{
    spCtx->bProbeInLayer.store(false);
    spCtx->bProbeDone.store(false);
    const bool bPosted = exec.Post(
        [spCtx]()
        {
            const bool bInLayer = (common::async::CurrentLayer() != NULL) || common::async::VisitLayerChain(
                                                                                 [](const CLayerInfo&)
                                                                                 {
                                                                                 });
            spCtx->bProbeInLayer.store(bInLayer);
            spCtx->bProbeDone.store(true);
        });
    ASSERT_TRUE(bPosted);
    ASSERT_TRUE(WaitFlag(spCtx->bProbeDone));
    ASSERT_TRUE(!spCtx->bProbeInLayer.load());
}

TEST(Trace_DeepChainAndFrameStackResidue)
{
    const int kExtraLayers = 255;          // 深链：链根 + 255 层 = 256 层
    CAsyncExecutor exec("trace-main", 1);  // **单线程**：探针必然复用同一条 worker（帧栈残留才查得出来）
    ASSERT_TRUE(exec.Start());
    const std::shared_ptr<CResidueCtx> spCtx = std::make_shared<CResidueCtx>();

    // ---- ① 深链：深度 / 链号 / 层号逐项对得上 ----
    spCtx->nLineRoot = __LINE__ + 1;
    CPromise<CResidueCtx> pDeep = exec.NewPromise(spCtx, &ResidueStepNoop, ASYNC_LOC);
    for (int i = 0; i < kExtraLayers - 1; ++i)
    {
        pDeep = pDeep.Then(&ResidueStepNoop, ASYNC_LOC);  // 同一行注册其余各层
    }
    spCtx->nLineDeep = __LINE__ + 1;
    pDeep = pDeep.Then(&ResidueStepDeep, ASYNC_LOC);
    ASSERT_TRUE(pDeep.Await().IsFulfilled());

    const std::vector<CLayerInfo>& vec = spCtx->capDeep.vecChain;
    ASSERT_EQ(vec.size(), static_cast<size_t>(kExtraLayers + 1));
    ASSERT_EQ(vec[0].loc.nLine, spCtx->nLineDeep);
    ASSERT_EQ(vec[vec.size() - 1].loc.nLine, spCtx->nLineRoot);
    for (size_t i = 0; i < vec.size(); ++i)
    {
        ASSERT_EQ(vec[i].nDepth, static_cast<int>(i));  // 深度 = 距当前层几跳
        ASSERT_TRUE(vec[i].bCurrent == (i == 0));       // 只有本层是「当前层」
        ASSERT_TRUE(vec[i].bSettled == (i != 0));       // 当前层按定义还没落定
        ASSERT_EQ(vec[i].nChainId, vec[0].nChainId);    // 一条链一个链号
        if (i > 0)
        {
            // 层号按创建顺序递增 → 往上游走必然严格递减（上游指针串了就会破坏它）。
            ASSERT_TRUE(vec[i].nLayerId < vec[i - 1].nLayerId);
        }
    }
    ASSERT_EQ(CountArrows(spCtx->capDeep.strChain), kExtraLayers);

    // ---- ①-2 dump 块：深链默认「头尾各半 + 中间省略」，可选全量（0 = 不限）----
    ASSERT_EQ(CountLines(spCtx->strDumpAll), kExtraLayers + 2);         // 头行 + 256 层（当前层 + 255 上游）
    ASSERT_TRUE(spCtx->strDumpAll.find("#255 ") != std::string::npos);  // 走到链根了
    ASSERT_TRUE(spCtx->strDumpAll.find("省略中间") == std::string::npos);
    ASSERT_EQ(CountLines(spCtx->strDumpDefault), common::async::kDumpMaxLayers + 2);  // 头行 + 省略行 + 上限层数
    ASSERT_TRUE(spCtx->strDumpDefault.find("省略中间") != std::string::npos);
    ASSERT_TRUE(spCtx->strDumpDefault.find("← 当前层") != std::string::npos);  // 当前层一定在最前

    // ---- ② 正常路径跑完 → 唯一 worker 上不该留帧 ----
    PostResidueProbe(exec, spCtx);

    // ---- ③ 异常路径：层内抛异常，帧同样必须弹干净 ----
    spCtx->nLineRoot = __LINE__ + 1;
    CPromise<CResidueCtx> pThrow = exec.NewPromise(spCtx, &ResidueStepNoop, ASYNC_LOC);
    pThrow = pThrow.Then(&ResidueStepNoop, ASYNC_LOC);
    pThrow = pThrow.Then(&ResidueStepThrow, ASYNC_LOC);
    const CPromiseResult rThrow = pThrow.Catch(&ResidueStepPassThrough, ASYNC_LOC).Await();
    ASSERT_TRUE(rThrow.IsRejected());
    ASSERT_TRUE(rThrow.Code() == static_cast<int>(common::async::kException));
    PostResidueProbe(exec, spCtx);

    // ---- ④ 上一条链的帧不影响下一条链：新链的链根只应看到自己 1 层 ----
    const std::shared_ptr<CResidueCtx> spAlone = std::make_shared<CResidueCtx>();
    spAlone->nLineRoot = __LINE__ + 1;
    CPromise<CResidueCtx> pAlone = exec.NewPromise(spAlone, &ResidueStepDeep, ASYNC_LOC);
    ASSERT_TRUE(pAlone.Await().IsFulfilled());
    const CExpect vecExpectAlone[1] = {{"then", spAlone->nLineRoot, "trace-main"}};
    AssertChain(spAlone->capDeep, vecExpectAlone, 1);

    // ---- ⑤ 主线程（调用方）从头到尾都不在任何层里 ----
    ASSERT_TRUE(common::async::CurrentLayer() == NULL);
    ASSERT_TRUE(!common::async::VisitLayerChain(
        [](const CLayerInfo&)
        {
        }));

    exec.Stop();
}

// ====================================================================
// 更复杂的场景（四）：层里 fire-and-forget 起的链，父层必须是**正在跑的那一层**
//
// 这条同时在查两件事：
//  ① 文档承诺的行为：层里起的链挂在「起它的那一层」下面；
//  ② 采纳作用域（`CChainAdopterScope`）必须**弹回**：前面那次 `ThenPromise` 留下的作用域
//     不能影响后面在层里起的链 —— 否则这里看到的父层就是**过期的那一层**（`CurrentLayerState()`
//     把显式作用域排在帧栈顶前面，作用域没弹回去就会一直劫持后续的起链）。
//
// 为什么要单线程执行器：让它俩（那次 adopt 与这次起链）落在**同一条线程**上，残留才暴露得出来。
// ====================================================================

/// @brief 「层里起链」用例的上下文。
struct CInLayerCtx
{
    CAsyncExecutor* pExec;  ///< 层里 fire-and-forget 起链要用（层处理器拿不到执行器就塞这儿）。
    int nLineRoot;          ///< 主链链根。
    int nLineAwait;         ///< 主链上等子链的那一层（`ThenPromise`，会压一次采纳作用域）。
    int nLineStarter;       ///< 主链上「在层里起旁支链」的那一层。
    int nLineSide;          ///< 旁支链自己的链根（采集点）。
    int nLineSubRoot;       ///< 子链链根（用来断言它不在旁支链的上游）。

    /// 旁支链跑完了吗。
    ///
    /// **故意不在这里存旁支链的句柄**：上下文会被每一层的处理器捕获（`spContext`），而层状态里的
    /// runner 又持着上下文 —— 上下文再存一个 promise 句柄，就「上下文 → 层状态 → runner → 上下文」
    /// 成环，整条链到进程退出都释放不掉（ASan 直接报出来；与 trace 无关，是「上下文里放句柄」
    /// 这个写法本身的问题）。所以这里只用个原子标志等它，不碰引用计数。
    std::atomic<bool> bSideDone;

    CCapture capSide;  ///< 旁支链链根的采集。

    CInLayerCtx()
        : pExec(NULL), nLineRoot(0), nLineAwait(0), nLineStarter(0), nLineSide(0), nLineSubRoot(0), bSideDone(false), capSide()
    {}
};

/// @brief 什么都不做的层。
CPromiseResult InLayerStepNoop(CPromiseResult upResult, const std::shared_ptr<CInLayerCtx>& spCtx)
{
    (void)upResult;
    (void)spCtx;
    return CPromiseResult::Resolve();
}

/// @brief 旁支链的链根：采集「自己 + 上游」——应该正好是「主链上起它的那一层」。
CPromiseResult InLayerStepSideRoot(CPromiseResult upResult, const std::shared_ptr<CInLayerCtx>& spCtx)
{
    (void)upResult;
    CaptureNow(spCtx->capSide);
    spCtx->bSideDone.store(true);  // 采集完了（原子标志，不碰引用计数）
    return CPromiseResult::Resolve();
}

/// @brief 在层里起一条**不等它**的链（fire-and-forget）。
CPromiseResult InLayerStepStarter(CPromiseResult upResult, const std::shared_ptr<CInLayerCtx>& spCtx)
{
    (void)upResult;
    // 句柄**故意丢掉**：投递出去的任务自己持着层状态，链会照跑；
    // 存进上下文反而会成环（见 `CInLayerCtx::bSideDone` 的说明）。
    // 注意下面两行必须紧挨着（`__LINE__ + 1` 就是给紧接着的挂层语句用的）。
    spCtx->nLineSide = __LINE__ + 1;
    spCtx->pExec->NewPromise(spCtx, &InLayerStepSideRoot, ASYNC_LOC);
    return CPromiseResult::Resolve();
}

TEST(Trace_StartedInsideLayerBindsToCurrentLayer)
{
    CAsyncExecutor exec("trace-main", 1);
    ASSERT_TRUE(exec.Start());
    const std::shared_ptr<CInLayerCtx> spCtx = std::make_shared<CInLayerCtx>();
    spCtx->pExec = &exec;

    // 子链工厂：目的只是让主链上出现一次 `ThenPromise`（也就是压一次采纳作用域）。
    const CPromise<CInLayerCtx>::PromiseFactory fnSub = [&exec, spCtx](const std::shared_ptr<CInLayerCtx>& spSelf)
    {
        spCtx->nLineSubRoot = __LINE__ + 1;
        return exec.NewPromise(spSelf, &InLayerStepNoop, ASYNC_LOC);
    };

    spCtx->nLineRoot = __LINE__ + 1;
    CPromise<CInLayerCtx> pRoot = exec.NewPromise(spCtx, &InLayerStepNoop, ASYNC_LOC);
    spCtx->nLineAwait = __LINE__ + 1;
    CPromise<CInLayerCtx> pAwait = pRoot.ThenPromise(fnSub, ASYNC_LOC);
    spCtx->nLineStarter = __LINE__ + 1;
    CPromise<CInLayerCtx> pStarter = pAwait.Then(&InLayerStepStarter, ASYNC_LOC);

    ASSERT_TRUE(pStarter.Await().IsFulfilled());

    // 旁支链已被投递（层里起链总是投递）：等它自己跑完，再看它采到的东西。
    ASSERT_TRUE(WaitFlag(spCtx->bSideDone));

    // 旁支链的祖先路径 = 自己 + 起它的那一层（starter）+ 主链剩余前缀 —— **不是**别的分支。
    // 注意顺序：starter 的上游就是那次 `ThenPromise` 的 await 层（主链是一条直线），
    // 所以「等待子链的层」本来就应该在路径上；不该出现的是**那条子链自己的链根**。
    const CExpect vecExpect[4] = {
        {"then", spCtx->nLineSide, "trace-main"},     // 旁支链自己的链根
        {"then", spCtx->nLineStarter, "trace-main"},  // 父层 = 正在跑的那一层
        {"then", spCtx->nLineAwait, "-"},             // 主链上等子链的那一层（桥接层，自己不跑 handler）
        {"then", spCtx->nLineRoot, "trace-main"},     // 主链链根
    };
    AssertChain(spCtx->capSide, vecExpect, 4);

    const std::vector<CLayerInfo>& vec = spCtx->capSide.vecChain;
    ASSERT_TRUE(vec[0].bChainRoot && vec[0].bSubChain);  // 层里起的链 = 子链（有父层）
    ASSERT_EQ(CountChainSegments(vec), 2);               // 旁支链一段 + 主链一段（父层之后都是主链）
    ASSERT_TRUE(vec[1].nDepth == 1 && !vec[1].bChainRoot && !vec[1].bSubChain);
    ASSERT_TRUE(!HasLine(spCtx->capSide, spCtx->nLineSubRoot));  // 子链是另一支，不在我的上游

    exec.Stop();
}


#endif  // defined(ASYNC_DEBUG_TRACE)
