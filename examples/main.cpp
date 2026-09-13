// ====================================================================
// 异步链完整示例：**一条父链**把所有常用用法串起来，并且每个位置都能看到自己的调用链
//
// 父链本体在 BuildOrderChain() 里，一行一层、一眼看完：
//
//   ① 起链（具名 handler）   exec.NewPromise(spCtx, &StepReadOrder, ASYNC_LOC)
//   ② then = lambda          只此一处用的小逻辑就写成 lambda
//   ③ then 就地              ThenInline：跑在「结算它的那条线程」上，省一次投递
//   ④ then 换执行器          ThenOn(execDb, ...)：换线程，**调用链不受影响**
//   ⑤ 内层链（同上下文）     ThenPromise：等一条自己搭的子链（2 层）
//   ⑥ 跨模块 / 跨上下文      ThenBridge：等别的模块（另一套 TContext），数据搬回来
//   ⑦ 分叉                   同一层挂两支：一支继续主线（⑨），一支旁支（⑧）
//   ⑧ 旁支 + fire-and-forget 层里 exec.Post(...)：链不等它（只写自己那一格）
//   ⑨ 主线继续               分叉的另一支
//   ⑩ 协程                   CO_AWAIT / CO_AWAIT_ALL（协程当父链的一步）
//   ⑪ then 收尾 / ⑫ catch 补偿 / ⑬ finally 审计
//
// 其余常用用法在 main() 里补齐：
//   - 组合器一族：WhenAll / WhenAllSettled / WhenRace / WhenAny（都在**执行器**上）；
//   - Await（阻塞取结果）/ AwaitFor(ms)（超时兜底）；
//   - OnSettled / OnSettledOn（settled 通知 —— **不是层**，不产生新层帧）；
//   - 回调式起链 exec.NewPromise(spCtx, fnStarter)（见记账模块 WriteBillAsync）；
//   - 拒绝路径：同一条父链再跑一遍（库存不足）→ 失败即停 + catch 补偿 + finally 审计。
//
// --------------------------------------------------------------------
// 追踪（本示例的重点）：每个位置都调 TraceHere(...)，打印「正在跑的层 + 一路往上的完整链」
//
//   [!] 想看追踪必须用**调试构建**（trace 与注册点 ASYNC_LOC 是同一个开关）：
//         ./build.sh --debug examples && ./build/debug/examples
//       发布构建下这些打印只剩一行「trace 关闭」的提示（接口是空操作，零开销）。
//
//   [i] 每层行尾的 tid 是「真正跑这一层的那条线程」，方括号里是「跑在哪个执行器上」
//       （`CAsyncExecutor("main", 8)` 这样的名字）—— 链跳执行器时一眼看得出跳到谁家去了。
//       同时 worker 线程也按「名字-序号」命名（`main-0`），gdb 的 `info threads` / htop
//       里可直接对应上。
//
//   [i] 子链 → 父链（已打通）：**起链时正在跑的那一层**会被记成新链「链根」的父层 ——
//       内层链（⑤）/ 跨模块子链（⑥）/ 协程起的子链（⑩）都能从子链里一路追回父链（连成一棵树）。
//       - 组合器（WhenAll 一族）：聚合层与各分支都挂在「发起它们的那一层」下面（多父一子）；
//       - 分叉：每条分支只看到「自己 + 共同上游」，看不到兄弟分支（下游不往上游走）。
// ====================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Assert.h"
#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/Trace.h"
#include "Coroutine/Coroutine.h"

using common::async::CAsyncExecutor;
using common::async::CCoroutine;
using common::async::CPromise;
using common::async::CPromiseResult;
#if defined(ASYNC_DEBUG_TRACE)
using common::async::CLayerInfo;
#endif

namespace {

// ====================================================================
// 一、通用小工具
// ====================================================================

/// 示例业务错误码（业务码从 kBusinessBase 起取）。
enum CErrCode
{
    kErrBadParam = common::async::kBusinessBase + 1,  ///< 参数非法。
    kErrNoStock = common::async::kBusinessBase + 2    ///< 库存不足。
};

/// @brief 睡一会儿（模拟 IO；示例里不真的连数据库）。
void SleepMs(int nMs)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(nMs));
}

/// @brief 起执行器并断言成功（动作在断言外：release 下断言不求值）。
void StartOrFail(CAsyncExecutor& exec)
{
    const bool bStarted = exec.Start();
    ASSERT(bStarted);
}

/// @brief 等一个原子计数到达期望值（有上限；超时返回 false）。
///
/// 用来等「层里 `exec.Post(...)` 出去的 fire-and-forget 任务」：框架保证它**最终**会跑完，
/// 但不保证它跑在**主链结束之前** —— 线程数越多，主链往往越先结束（协程里那两条并行等待
/// 会真的并行）。所以这类断言必须先等、再断言，不能靠时序侥幸。
///
/// @param nCounter 原子计数（旁支任务自己加）。
/// @param nExpect 期望值。
/// @param nMaxMs 最长等多久（毫秒）。
/// @return true = 等到了。
bool WaitCount(const std::atomic<int>& nCounter, int nExpect, int nMaxMs = 2000)
{
    for (int i = 0; i < nMaxMs / 5; ++i)
    {
        if (nCounter.load() >= nExpect)
        {
            return true;
        }
        SleepMs(5);
    }
    return nCounter.load() >= nExpect;
}

#if defined(ASYNC_DEBUG_TRACE)

/// @brief 数一数当前层能看到的链有几层。
int CountChain()
{
    int nCount = 0;
    common::async::VisitLayerChain(
        [&nCount](const CLayerInfo&)
        {
            ++nCount;
        });
    return nCount;
}

#endif  // defined(ASYNC_DEBUG_TRACE)

/// 只有调试构建才存在的动作（发布构建根本没有 trace）。
///
/// 写成宏是为了让两份构建共用同一套处理器代码（否则每个处理器里都要写一段 `#if`）。
#if defined(ASYNC_DEBUG_TRACE)
    #define TRACE_ONLY(stmt) stmt
#else
    #define TRACE_ONLY(stmt) ((void)0)
#endif

/// 某个参数只在调试构建的 trace 代码里用到时，用它「消费」一下。
///
/// 发布构建下 `TRACE_ONLY(...)` 展开为空，参数就没人碰了，编译器会报
/// `-Wunused-parameter`；这里补一次无害的读取。
#if defined(ASYNC_DEBUG_TRACE)
    #define TRACE_USE(x) ((void)0)
#else
    #define TRACE_USE(x) ((void)(x))
#endif

/// @brief 打印「当前层 + 一路往上的完整调用链」—— 本示例要给你看的就是这个。
///
/// 分叉 / 协程并行 await 会让多个工作线程同时打印，所以这里加锁，免得几段输出绞在一起。
///
/// @param pszWhere 这是哪一步（① … ⑬）。
void TraceHere(const char* pszWhere)
{
    static std::mutex s_mutex;  // 只为让本示例的输出好看

    std::lock_guard<std::mutex> lock(s_mutex);
    std::printf("\n  [%s]\n", pszWhere);

#if defined(ASYNC_DEBUG_TRACE)
    const CLayerInfo* pInfo = common::async::CurrentLayer();
    if (pInfo == NULL)
    {
        std::printf("      当前不在任何层里（通知 / Post 的旁支任务 / 主线程都看不到层）\n");
        return;
    }

    // 一行一层：`DescribeLayer` 把「注册点 + 链号/层号 + 线程 + 耗时 + 结果」都拼好了。
    std::printf("      本层 + 完整链 %d 层（近 → 远 = 从本层往上游追，谁挂的它）\n", CountChain());
    common::async::VisitLayerChain(
        [](const CLayerInfo& info)
        {
            std::printf("        %s\n", common::async::DescribeLayer(info).c_str());
        });
#else
    // 发布构建没有 trace（取链的接口根本不存在）—— 每个位置都打这句就太吵了，只提示一次。
    static bool s_bNoticed = false;
    if (!s_bNoticed)
    {
        s_bNoticed = true;
        std::printf("      （发布构建：trace 只在调试构建存在 —— 用 ./build/debug/examples 跑，这里会打出完整调用链）\n");
    }
#endif
}

/// @brief 链里有没有某个注册点（用来断言「某一层不在我的链上」）。
///
/// 只在调试构建用得上（发布构建下 trace 是空操作、断言也不看链），所以跟着开关走。
#if defined(ASYNC_DEBUG_TRACE)
bool HasChainLine(const std::vector<CLayerInfo>& vecChain, int nLine)
{
    for (size_t i = 0; i < vecChain.size(); ++i)
    {
        if (vecChain[i].loc.nLine == nLine)
        {
            return true;
        }
    }
    return false;
}
#endif  // defined(ASYNC_DEBUG_TRACE)

// ====================================================================
// 二、父链的共享上下文与各层
// ====================================================================

/// @brief 父链共享上下文：**层间只传兑现 / 拒绝，数据一律放这里**。
///
/// 并行分支（⑧ 物流 / ⑨ 优惠券、协程并行 await 的两条子链）只能各写**不同字段** ——
/// 框架只保证同一条链上的层顺序执行，跨链并发由调用方负责。
struct COrderCtx
{
    // ---- 输入 ----
    int nQty;         ///< 数量（模拟输入）。
    bool bFailStock;  ///< true = 模拟「库存不足」（走拒绝路径）。

    // ---- 各层产出 ----
    long nOrderId;         ///< 订单号（①）。
    int nStock;            ///< 库存（③）。
    int nDiscount;         ///< 折扣金额（⑤-1）。
    int nTotal;            ///< 总额（⑤-2 算出来；⑪ 再减优惠券）。
    int nBillNo;           ///< 账单号（⑥ 跨模块取回）。
    int nCoupon;           ///< 优惠券（⑨）。
    int nGift;             ///< 赠品数（⑩-2a）。
    int nPoints;           ///< 积分（⑩-2b）。
    int nLogistics;        ///< 物流标记（⑧ 旁支，只写自己这一格）。
    std::string strTrace;  ///< 顺序层留下的轨迹（自校验；并行分支一律不碰它）。

    // ---- 跨线程计数（并行分支 / Post 旁支用） ----
    std::atomic<int> nSideDone;  ///< Post 出去的旁支任务完成数。
    std::atomic<int> nCoroDone;  ///< 协程并行 await 的两条子链完成数。

    // ---- 跨作用域持有句柄 / 保活 ----
    // CPromise 没有默认构造，要跨作用域持有就用 shared_ptr<CPromise<Ctx>> 装（框架推荐写法）；
    // 协程对象也必须活到完成，所以同样挂在这里。
    std::shared_ptr<void> spCoroHold;                  ///< 协程对象保活。
    std::shared_ptr<CPromise<COrderCtx> > spSideHold;  ///< 分叉旁支（⑧）的句柄。

    // ---- 追踪自校验（由各层填，主线程断言；其中 vecAuditChain 只在调试构建存在） ----
    int nAuditChain;       ///< ⑬ 看到的链层数。
    int nSideChain;        ///< ⑧ 看到的链层数。
    int nInnerChainSeen;   ///< ⑤ 内层链里看到的层数。
    int nModuleChainSeen;  ///< ⑥ 跨模块子链里看到的层数。
    int nCoroChainSeen;    ///< ⑩-1 协程起的子链里看到的层数。
#if defined(ASYNC_DEBUG_TRACE)
    std::vector<CLayerInfo> vecAuditChain;  ///< ⑬ 看到的完整链快照。
#endif

    COrderCtx()
        : nQty(0),
          bFailStock(false),
          nOrderId(0),
          nStock(0),
          nDiscount(0),
          nTotal(0),
          nBillNo(0),
          nCoupon(0),
          nGift(0),
          nPoints(0),
          nLogistics(0),
          strTrace(),
          nSideDone(0),
          nCoroDone(0),
          spCoroHold(),
          spSideHold(),
          nAuditChain(0),
          nSideChain(0),
          nInnerChainSeen(0),
          nModuleChainSeen(0),
          nCoroChainSeen(0)
    {}
};

#if defined(ASYNC_DEBUG_TRACE)
/// @brief 把「当前层 + 一路往上的完整链」抄进上下文（⑬ 审计层用它做全链自校验）。
///
/// 抄下来是为了**离开层之后**再断言：层里只能看，主线程要等两条链都跑完才核对。
///
/// @param spCtx 父链上下文（快照写进 vecAuditChain / nAuditChain）。
void CaptureAuditChain(const std::shared_ptr<COrderCtx>& spCtx)
{
    spCtx->vecAuditChain.clear();
    common::async::VisitLayerChain(
        [spCtx](const CLayerInfo& info)
        {
            spCtx->vecAuditChain.push_back(info);
        });
    spCtx->nAuditChain = static_cast<int>(spCtx->vecAuditChain.size());
}
#endif  // defined(ASYNC_DEBUG_TRACE)

/// 层 ①：读订单（具名 handler；本层就是链根 —— 它没有上游）。
CPromiseResult StepReadOrder(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    (void)upResult;
    TraceHere("① 读订单（具名 handler，链根）");
    spCtx->nOrderId = 1001;
    spCtx->strTrace += "读订单;";
    return CPromiseResult::Resolve();
}

/// 层 ③：查库存。`ThenInline` = 就地执行（不投递，跑在「结算它的那条线程」上）。
CPromiseResult StepCheckStock(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    (void)upResult;
    TraceHere("③ 查库存（ThenInline：就地，不投递）");
    spCtx->strTrace += "查库存;";
    if (spCtx->bFailStock)
    {
        return CPromiseResult::Reject(kErrNoStock);  // 失败即停：④…⑪ 都不执行
    }
    spCtx->nStock = 5;
    return CPromiseResult::Resolve();
}

/// 层 ④：读用户。`ThenOn(execDb, ...)` = 这一层在**别的执行器**上跑（换了线程）。
CPromiseResult StepLoadUser(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    (void)upResult;
    TraceHere("④ 读用户（ThenOn：换到 execDb 的线程上跑）");
    SleepMs(2);  // 模拟一次数据访问
    spCtx->strTrace += "读用户;";
    return CPromiseResult::Resolve();
}

/// 层 ⑤-1：算折扣（内层链第 1 层）。
CPromiseResult StepCalcDiscount(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    (void)upResult;
    TraceHere("⑤-1 算折扣（内层链第 1 层）");
    spCtx->nDiscount = 30;
    spCtx->strTrace += "算折扣;";
    return CPromiseResult::Resolve();
}

/// 层 ⑤-2：算总额（内层链最深一层 —— 它只看得到内层链自己）。
CPromiseResult StepApplyDiscount(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    (void)upResult;
    TraceHere("⑤-2 算总额（内层链最深）");
    spCtx->nTotal = spCtx->nQty * 100 - spCtx->nDiscount;
    TRACE_ONLY(spCtx->nInnerChainSeen = CountChain());  // 内层链能追回主链（父层 = ⑤ 那一层）
    spCtx->strTrace += "算总额;";
    return CPromiseResult::Resolve();
}

/// 层 ⑦：分叉基座（什么都不做，只为了让两支有共同的上游）。
CPromiseResult StepForkBase(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    (void)upResult;
    TraceHere("⑦ 分叉基座（下面两支：⑧ 旁支 / ⑨ 主线）");
    spCtx->strTrace += "分叉;";
    return CPromiseResult::Resolve();
}

/// 层 ⑨：优惠券（分叉的另一支，主线从这里继续）。
CPromiseResult StepCoupon(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    (void)upResult;
    TraceHere("⑨ 优惠券（分叉的一支：主线继续走这支）");
    spCtx->nCoupon = 5;
    spCtx->strTrace += "优惠券;";
    return CPromiseResult::Resolve();
}

/// 层 ⑩-1：协程顺序 await 的那条子链。
CPromiseResult StepCheckCoupon(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    (void)upResult;
    TraceHere("⑩-1 协程 CO_AWAIT 的子链（挂在启动协程的那一层下面）");
    TRACE_ONLY(spCtx->nCoroChainSeen = CountChain());
    spCtx->strTrace += "协程券;";
    return CPromiseResult::Resolve();
}

/// 层 ⑩-2a：协程并行 await 的两条子链之一（各写各的字段 —— 并行不碰 strTrace）。
CPromiseResult StepCheckGift(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    (void)upResult;
    TraceHere("⑩-2a 协程 CO_AWAIT_ALL 的子链之一");
    SleepMs(3);
    spCtx->nGift = 1;
    spCtx->nCoroDone.fetch_add(1);
    return CPromiseResult::Resolve();
}

/// 层 ⑩-2b：协程并行 await 的另一条子链。
CPromiseResult StepCheckPoints(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    (void)upResult;
    TraceHere("⑩-2b 协程 CO_AWAIT_ALL 的子链之二");
    SleepMs(3);
    spCtx->nPoints = 20;
    spCtx->nCoroDone.fetch_add(1);
    return CPromiseResult::Resolve();
}

/// 层 ⑪：落库收尾（把优惠券算进总额）。
CPromiseResult StepSettle(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    (void)upResult;
    TraceHere("⑪ 落库（then 收尾）");
    spCtx->nTotal -= spCtx->nCoupon;
    spCtx->strTrace += "落库;";
    return CPromiseResult::Resolve();
}

/// 层 ⑫：`Catch` 补偿 —— 只在**被拒绝**时执行（成功路径上它在链里，但不执行）。
///
/// 这里**原样透传**上一层结果：补偿完仍然让链保持拒绝（调用方看得到失败）。
/// 若返回 `Resolve()` 就是「吞掉拒绝、恢复链」，后面还能继续挂 then。
CPromiseResult StepCompensate(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    TraceHere("⑫ 补偿（catch：仅被拒绝时执行）");
    std::printf("      拒绝码=%d（业务码）\n", upResult.Code());
    spCtx->nStock = 0;
    spCtx->strTrace += "补偿;";
    return upResult;
}

/// 层 ⑬：`Finally` 审计 —— 无论成败都执行、不改结果；它最深，正好看整条链。
CPromiseResult StepAudit(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    TraceHere("⑬ 审计（finally：最深的一层，能看到整条链）");

    TRACE_ONLY(CaptureAuditChain(spCtx));  // ⑬ 最深，把整条链抄下来（只有调试构建有 trace）
    spCtx->strTrace += "审计;";

    // 审计层本来就不该改结果：原样透传（finally 的「忽略返回值」由框架保证，这里写出来更直白）。
    return upResult;
}

// ====================================================================
// 三、跨模块：记账模块（自持执行器 + 自持上下文）
// ====================================================================

/// @brief 记账模块：示例里的「另一个模块」。
///
/// 要点（真实项目照这个来）：
///  - **自持执行器**：跨模块只交换 promise，不传递执行器；
///  - 自持上下文类型（`CBillCtx`），与父链的 `COrderCtx` 不同 → 演示跨上下文桥接；
///  - **回调式起链**：`exec.NewPromise(spCtx, fnStarter)`，由外部完成回调 settle 本链。
class CBillingModule
{
public:
    /// @brief 账单子链的上下文（别的模块自己的数据，父链看不到）。
    struct CBillCtx
    {
        long nOrderId;   ///< 订单号（调用方给的）。
        int nTotal;      ///< 金额（调用方给的）。
        int nBillNo;     ///< 账单号（本模块生成）。
        int nChainSeen;  ///< 本模块链上看到的层数（演示「跨模块子链能追回父链」）。

        CBillCtx() : nOrderId(0), nTotal(0), nBillNo(0), nChainSeen(0)
        {}
    };

    CBillingModule() : m_exec("billing", 4), m_nNextBillNo(1000)
    {}

    /// @brief 起模块（执行器启动失败返回 false）。
    bool Start()
    {
        return m_exec.Start();
    }

    /// @brief 停模块（等正在跑的任务收尾）。
    void Stop()
    {
        m_exec.Stop();
    }

    /// @brief 对外异步函数：写一条账单。
    ///
    /// 回调式起链：`fnStarter` 里发起异步动作（真实场景是 IO / RPC），完成时调
    /// `fnResolve()` / `fnReject(码)`；它只登记动作，绝不阻塞调用方线程。
    ///
    /// @param nOrderId 订单号。
    /// @param nTotal 金额。
    /// @return 子链句柄（调用方用 `ThenBridge` 等它）。
    CPromise<CBillCtx> WriteBillAsync(long nOrderId, int nTotal)
    {
        const std::shared_ptr<CBillCtx> spCtx = std::make_shared<CBillCtx>();
        spCtx->nOrderId = nOrderId;
        spCtx->nTotal = nTotal;
        const int nBillNo = m_nNextBillNo++;

        CPromise<CBillCtx>::ChainStarter fnStarter =
            [this, spCtx, nBillNo](const CPromise<CBillCtx>::ResolveFn& fnResolve, const CPromise<CBillCtx>::RejectFn& fnReject)
        {
            // 真实场景：这里发起异步 IO（只登记回调）；示例用「投递 + 延时」模拟对方的完成通知。
            const bool bPosted = m_exec.Post(
                [spCtx, nBillNo, fnResolve]()
                {
                    SleepMs(2);
                    spCtx->nBillNo = nBillNo;  // 写子上下文
                    fnResolve();               // 兑现子链
                });
            if (!bPosted)
            {
                fnReject(common::async::kStopped);  // 模块已停：别让子链永远挂着
            }
        };

        // 模块自己的链：第 1 层（由外部回调 settle）+ 第 2 层（留痕）。
        const CPromise<CBillCtx>::ThenHandler fnBillAudit = [](CPromiseResult upResult, const std::shared_ptr<CBillCtx>& spSelf)
        {
            (void)upResult;
            TRACE_USE(spSelf);  // 发布构建下 spSelf 只被 trace 代码用到
            TraceHere("⑥-2 记账模块自己的链（跨模块子链：能追回父链）");
            TRACE_ONLY(spSelf->nChainSeen = CountChain());
            return CPromiseResult::Resolve();
        };
        return m_exec
            .NewPromise(spCtx, fnStarter, ASYNC_LOC)  //
            .Then(fnBillAudit, ASYNC_LOC);
    }

private:
    CAsyncExecutor m_exec;           ///< 模块自持执行器（不跨模块传递）。
    std::atomic<int> m_nNextBillNo;  ///< 账单号自增。
};

// ====================================================================
// 四、协程（⑩）：协程当父链的一步
// ====================================================================

/// @brief 协程：顺序 await 一条子链 + 并行 await 两条子链。
///
/// await 不传数据（数据走共享上下文 `GetContext()`）；`NewPromise` 起的是**子链**，
/// 所以子链里的层看到的是子链自己那条（看示例输出的 ⑩-1 / ⑩-2）。
class CStepCoroutine : public CCoroutine<COrderCtx>
{
public:
    explicit CStepCoroutine(const std::shared_ptr<COrderCtx>& spCtx) : CCoroutine<COrderCtx>(spCtx)
    {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepCheckCoupon, ASYNC_LOC));  // 顺序 await
        CO_AWAIT_ALL(                                       //
            NewPromise(&StepCheckGift, ASYNC_LOC),          //
            NewPromise(&StepCheckPoints, ASYNC_LOC));       // 并行 await
        CO_RETURN_VOID();
        CO_END();
    }
};

// ====================================================================
// 五、父链本体：一条链走完 ① … ⑬
// ====================================================================

/// @brief 父链各层的注册点（`__LINE__ + 1` 逐个采集，用于「行号对得上」的自校验）。
///
/// 采集条件：挂层语句必须**整行**（`ASYNC_LOC` 与调用在同一行上）—— 多行实参会把
/// `ASYNC_LOC` 挤到第二行，记下的是那一行。所以 lambda / 工厂都先变成具名变量再挂。
struct CLines
{
    int nReadOrder;   ///< ①
    int nValidate;    ///< ②
    int nCheckStock;  ///< ③
    int nLoadUser;    ///< ④
    int nPricing;     ///< ⑤
    int nBilling;     ///< ⑥
    int nForkBase;    ///< ⑦
    int nSide;        ///< ⑧
    int nCoupon;      ///< ⑨
    int nCoroutine;   ///< ⑩
    int nSettle;      ///< ⑪
    int nCompensate;  ///< ⑫
    int nAudit;       ///< ⑬

    CLines()
        : nReadOrder(0),
          nValidate(0),
          nCheckStock(0),
          nLoadUser(0),
          nPricing(0),
          nBilling(0),
          nForkBase(0),
          nSide(0),
          nCoupon(0),
          nCoroutine(0),
          nSettle(0),
          nCompensate(0),
          nAudit(0)
    {}
};

/// @brief 搭一条「下单」父链：所有常用用法都在这一条链上（每个位置都会打印调用链）。
///
/// @param execMain 主链执行器（多线程：分叉两支与协程并行 await 都会真的并发跑）。
/// @param execDb 模拟「数据访问模块」的执行器（`ThenOn` 换线程用）。
/// @param billing 记账模块（`ThenBridge` 跨模块 / 跨上下文用）。
/// @param spCtx 共享上下文（各层读写它；层间只传成败）。
/// @param lines 出口：各层注册点行号（自校验用）。
/// @return 父链末尾句柄（`Catch` → `Finally`）。
CPromise<COrderCtx> BuildOrderChain(CAsyncExecutor& execMain, CAsyncExecutor& execDb, CBillingModule& billing,
    const std::shared_ptr<COrderCtx>& spCtx, CLines& lines)
{
    //---------------- 先把「写成 lambda 的层」与「工厂」变成具名变量 ----------------

    /// ② 校验参数：只此一处用的小逻辑 → 写成 lambda。
    const CPromise<COrderCtx>::ThenHandler fnValidate = [](CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spSelf)
    {
        (void)upResult;
        TraceHere("② 校验参数（then = lambda）");
        if (spSelf->nQty <= 0)
        {
            return CPromiseResult::Reject(kErrBadParam);
        }
        spSelf->strTrace += "校验;";
        return CPromiseResult::Resolve();
    };

    /// ⑤ 内层链工厂：返回一条**自己搭的子链**（同上下文 → 直接 adopt 进当前链）。
    const CPromise<COrderCtx>::PromiseFactory fnPricingChain = [&execMain](const std::shared_ptr<COrderCtx>& spSelf)
    {
        return execMain
            .NewPromise(spSelf, &StepCalcDiscount, ASYNC_LOC)  // ⑤-1 算折扣
            .Then(&StepApplyDiscount, ASYNC_LOC);              // ⑤-2 算总额
    };

    /// ⑥ 跨模块「起子链」：在别的模块的执行器上跑，另一套上下文。
    const std::function<CPromise<CBillingModule::CBillCtx>(const std::shared_ptr<COrderCtx>&)> fnCreateBill =
        [&billing](const std::shared_ptr<COrderCtx>& spSelf)
    {
        return billing.WriteBillAsync(spSelf->nOrderId, spSelf->nTotal);
    };

    /// ⑥ 跨模块「搬数据」：跑在**子链的结算线程**上，只搬数据、别碰本模块状态。
    const std::function<void(const std::shared_ptr<COrderCtx>&, const std::shared_ptr<CBillingModule::CBillCtx>&)> fnApplyBill =
        [](const std::shared_ptr<COrderCtx>& spSelf, const std::shared_ptr<CBillingModule::CBillCtx>& spChild)
    {
        spSelf->nBillNo = spChild->nBillNo;
        spSelf->nModuleChainSeen = spChild->nChainSeen;
        spSelf->strTrace += "记账;";
    };

    /// ⑧ 分叉旁支：物流。层里再 Post 一个**不等它**的旁支任务（fire-and-forget）。
    const CPromise<COrderCtx>::ThenHandler fnLogistics = [&execMain](
                                                             CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spSelf)
    {
        (void)upResult;
        TraceHere("⑧ 物流支（分叉的一支）");
        const bool bPosted = execMain.Post(
            [spSelf]()
            {
                SleepMs(5);  // 旁支里的重活
                spSelf->nSideDone.fetch_add(1);
            });
        ASSERT(bPosted);  // 返回 false = 执行器已停 / 空任务
        spSelf->nLogistics = 1;
        TRACE_ONLY(spSelf->nSideChain = CountChain());  // 旁支看得到「自己 + 共同上游」，看不到下游
        return CPromiseResult::Resolve();
    };

    /// ⑩ 协程当一步：起协程并返回它的 promise（由父链等它）。
    ///     协程对象要活到完成 → 挂在上下文里（`spCoroHold` 是 `std::shared_ptr<void>`）。
    const CPromise<COrderCtx>::PromiseFactory fnCoroutineStep = [&execMain](const std::shared_ptr<COrderCtx>& spSelf)
    {
        const std::shared_ptr<CStepCoroutine> pCoro = execMain.CoStart<CStepCoroutine>(spSelf);
        spSelf->spCoroHold = pCoro;
        return pCoro->AsPromise();
    };

    //---------------- 父链本体：一行一层，行尾注释就是步骤号 ----------------

    lines.nReadOrder = __LINE__ + 1;
    CPromise<COrderCtx> pChain = execMain.NewPromise(spCtx, &StepReadOrder, ASYNC_LOC);  // ① 起链（具名 handler）
    lines.nValidate = __LINE__ + 1;
    pChain = pChain.Then(fnValidate, ASYNC_LOC);  // ② then = lambda
    lines.nCheckStock = __LINE__ + 1;
    pChain = pChain.ThenInline(&StepCheckStock, ASYNC_LOC);  // ③ then 就地
    lines.nLoadUser = __LINE__ + 1;
    pChain = pChain.ThenOn(execDb, &StepLoadUser, ASYNC_LOC);  // ④ then 换执行器
    lines.nPricing = __LINE__ + 1;
    pChain = pChain.ThenPromise(fnPricingChain, ASYNC_LOC);  // ⑤ 内层链（等它）
    lines.nBilling = __LINE__ + 1;
    pChain = pChain.ThenBridge(fnCreateBill, fnApplyBill, ASYNC_LOC);  // ⑥ 跨模块 / 跨上下文
    lines.nForkBase = __LINE__ + 1;
    CPromise<COrderCtx> pBase = pChain.Then(&StepForkBase, ASYNC_LOC);  // ⑦ 分叉基座
    lines.nSide = __LINE__ + 1;
    const CPromise<COrderCtx> pSide = pBase.Then(fnLogistics, ASYNC_LOC);  // ⑧ 分叉支（旁支，单独等它）
    lines.nCoupon = __LINE__ + 1;
    pChain = pBase.Then(&StepCoupon, ASYNC_LOC);  // ⑨ 分叉的另一支（主线继续）
    lines.nCoroutine = __LINE__ + 1;
    pChain = pChain.ThenPromise(fnCoroutineStep, ASYNC_LOC);  // ⑩ 协程当一步
    lines.nSettle = __LINE__ + 1;
    pChain = pChain.Then(&StepSettle, ASYNC_LOC);  // ⑪ then 收尾
    lines.nCompensate = __LINE__ + 1;
    pChain = pChain.Catch(&StepCompensate, ASYNC_LOC);  // ⑫ catch 补偿（成功路径被跳过）
    lines.nAudit = __LINE__ + 1;
    pChain = pChain.Finally(&StepAudit, ASYNC_LOC);  // ⑬ finally 审计（最深）

    // 旁支也得有人持有并等它（否则断言物流结果时它可能还没跑完）。
    spCtx->spSideHold = std::make_shared<CPromise<COrderCtx> >(pSide);
    return pChain;
}

// ====================================================================
// 六、组合器一族（都在**执行器**上）
// ====================================================================

/// @brief 起一条「一步就完」的小链（组合器演示用）。
///
/// @param exec 执行器。
/// @param spCtx 上下文。
/// @param bReject true = 这一层拒绝。
/// @param nCode 拒绝码（bReject 为 true 时用）。
/// @param nSleepMs 这一层耗时（用来做 Race / Any 的先后）。
CPromise<COrderCtx> MakeQuickChain(
    CAsyncExecutor& exec, const std::shared_ptr<COrderCtx>& spCtx, bool bReject, int nCode, int nSleepMs)
{
    const CPromise<COrderCtx>::ThenHandler fnStep = [bReject, nCode, nSleepMs](
                                                        CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spSelf)
    {
        (void)upResult;
        (void)spSelf;
        SleepMs(nSleepMs);
        if (bReject)
        {
            return CPromiseResult::Reject(nCode);
        }
        return CPromiseResult::Resolve();
    };
    return exec.NewPromise(spCtx, fnStep, ASYNC_LOC);
}

/// @brief 组合器一族：WhenAll / WhenAllSettled / WhenRace / WhenAny。
void DemoCombinators(CAsyncExecutor& execMain)
{
    std::printf("\n==================== 组合器一族（聚合层没有上游 → 追踪到此为止）====================\n");

    // WhenAll：全部兑现才继续；任一拒绝 → 立即失败。
    {
        const std::shared_ptr<COrderCtx> spCtx = std::make_shared<COrderCtx>();
        const CPromiseResult r =
            execMain.WhenAll(spCtx, MakeQuickChain(execMain, spCtx, false, 0, 5), MakeQuickChain(execMain, spCtx, false, 0, 5))
                .Await();
        ASSERT(r.IsFulfilled());
        std::printf("  WhenAll        （2 条都兑现）→ 兑现\n");
    }

    // WhenAllSettled：全部落定即继续（不看成败）。
    {
        const std::shared_ptr<COrderCtx> spCtx = std::make_shared<COrderCtx>();
        const CPromiseResult r =  //
            execMain
                .WhenAllSettled(spCtx,                             //
                    MakeQuickChain(execMain, spCtx, false, 0, 5),  //
                    MakeQuickChain(execMain, spCtx, true, kErrNoStock, 5))
                .Await();
        ASSERT(r.IsFulfilled());  // 有一支被拒绝，聚合层照样兑现
        std::printf("  WhenAllSettled （1 兑现 + 1 拒绝）→ 兑现（不看成败）\n");
    }

    // WhenRace：第一个**落定**的结果就是聚合结果（先到先得）。
    {
        const std::shared_ptr<COrderCtx> spCtx = std::make_shared<COrderCtx>();
        const CPromiseResult r =  //
            execMain
                .WhenRace(spCtx,                                            //
                    MakeQuickChain(execMain, spCtx, true, kErrNoStock, 0),  //
                    MakeQuickChain(execMain, spCtx, false, 0, 60))          //
                .Await();
        ASSERT(r.IsRejected());
        ASSERT(r.Code() == static_cast<int>(kErrNoStock));  // 快的那支（拒绝）先到
        std::printf("  WhenRace       （快=拒绝 0ms / 慢=兑现 60ms）→ 拒绝(码=%d)：先到先得\n", r.Code());
    }

    // WhenAny：第一个**兑现**的才是聚合结果（全被拒才拒绝）。
    {
        const std::shared_ptr<COrderCtx> spCtx = std::make_shared<COrderCtx>();
        const CPromiseResult r = execMain
                                     .WhenAny(spCtx, MakeQuickChain(execMain, spCtx, true, kErrNoStock, 0),
                                         MakeQuickChain(execMain, spCtx, false, 0, 60))
                                     .Await();
        ASSERT(r.IsFulfilled());  // 先到的是拒绝 → 不算，继续等兑现
        std::printf("  WhenAny        （快=拒绝 0ms / 慢=兑现 60ms）→ 兑现（只认第一个兑现者）\n");
    }
}

// ====================================================================
// 七、入口
// ====================================================================

/// @brief 打印一条父链的最终结果（成功 / 拒绝两条路径共用）。
void PrintResult(const char* pszPath, const CPromiseResult& r, const std::shared_ptr<COrderCtx>& spCtx)
{
    std::printf("\n  ── %s：结果=%s 拒绝码=%d\n", pszPath, r.IsFulfilled() ? "兑现" : "拒绝", r.Code());
    std::printf("     订单=%ld 库存=%d 折扣=%d 总额=%d 账单=%d 优惠券=%d 赠品=%d 积分=%d 物流=%d\n", spCtx->nOrderId,
        spCtx->nStock, spCtx->nDiscount, spCtx->nTotal, spCtx->nBillNo, spCtx->nCoupon, spCtx->nGift, spCtx->nPoints,
        spCtx->nLogistics);
    std::printf("     轨迹=%s\n", spCtx->strTrace.c_str());
}

/// @brief 断言 ⑬（finally）看到的那条链与期望逐项一致（近 → 远）。
void AssertAuditChain(const std::shared_ptr<COrderCtx>& spCtx, const CLines& lines)
{
#if defined(ASYNC_DEBUG_TRACE)
    // 近 → 远：⑬ finally → ⑫ catch → ⑪ ⑩ ⑨ ⑦ ⑥ ⑤ ④ ③ ② ①（⑧ 是分叉的另一支，不在主线上）
    ASSERT(spCtx->nAuditChain == 12);
    ASSERT(spCtx->vecAuditChain.size() == static_cast<size_t>(spCtx->nAuditChain));

    const int vecExpect[12] = {lines.nAudit, lines.nCompensate, lines.nSettle, lines.nCoroutine, lines.nCoupon, lines.nForkBase,
        lines.nBilling, lines.nPricing, lines.nLoadUser, lines.nCheckStock, lines.nValidate, lines.nReadOrder};
    for (int i = 0; i < 12; ++i)
    {
        const CLayerInfo& info = spCtx->vecAuditChain[static_cast<size_t>(i)];
        ASSERT(info.loc.nLine == vecExpect[i]);  // 每一层的注册点都对得上
        ASSERT(info.nDepth == i);                // 深度 = 距当前层几跳
        ASSERT(info.bCurrent == (i == 0));       // 只有第一项是「当前层」
    }
    ASSERT(spCtx->vecAuditChain[0].eMode == common::async::detail::kModeFinally);
    ASSERT(spCtx->vecAuditChain[1].eMode == common::async::detail::kModeCatch);
    ASSERT(spCtx->vecAuditChain[11].eMode == common::async::detail::kModeThen);  // 链根

    // 分叉：兄弟分支不在我的链上（只往上游走）。
    ASSERT(!HasChainLine(spCtx->vecAuditChain, lines.nSide));

    // 子链 → 父链：子链的链根挂在「起它的那一层」下面，所以从子链里能一路追回父链。
    ASSERT(spCtx->nInnerChainSeen == 7);   // ⑤ 内层链：自己 2 层 + ⑤④③②①
    ASSERT(spCtx->nModuleChainSeen == 8);  // ⑥ 记账模块的链：自己 2 层 + ⑥⑤④③②①
    ASSERT(spCtx->nCoroChainSeen == 10);   // ⑩-1 协程起的子链：自己 + ⑩⑨⑦⑥⑤④③②①
    ASSERT(spCtx->nSideChain == 8);        // ⑧ 旁支：自己 + 共同上游 7 层（看不到下游）

    std::printf(
        "\n  追踪自校验：⑬ 看到 %d 层完整链；⑧ 旁支 %d 层；子链也能追回父链（⑤ 内层 %d 层 / ⑥ 跨模块 %d 层 / ⑩ 协程 %d 层）\n",
        spCtx->nAuditChain, spCtx->nSideChain, spCtx->nInnerChainSeen, spCtx->nModuleChainSeen, spCtx->nCoroChainSeen);
#else
    // 发布构建：trace 只在调试构建存在（没有取链的接口，也就没链可查），这里只确认
    // 「没人往里填过数」——链相关的字段全是 0。
    (void)lines;
    ASSERT(spCtx->nAuditChain == 0);
#endif
}

}  // namespace

int main()
{
    std::printf("==================== 一条父链走完所有常用异步用法 ====================\n");
#if defined(ASYNC_DEBUG_TRACE)
    std::printf("调试构建：每个位置都会打印「完整调用链」（近 → 远 = 从本层往上游追，谁挂的它）\n");
#else
    std::printf("发布构建：trace 关闭（用 ./build.sh --debug examples 跑，才能看到每层的调用链）\n");
#endif

    CAsyncExecutor execMain("main", 8);  // 主链：8 线程（分叉两支 / 协程并行 await 会**真的同时**跑）
    CAsyncExecutor execDb("db", 4);      // 模拟「数据访问模块」自己的执行器
    CBillingModule billing;              // 记账模块（自持执行器 + 自持上下文，名字 billing）
    StartOrFail(execMain);
    StartOrFail(execDb);
    const bool bBillStarted = billing.Start();
    ASSERT(bBillStarted);

    //==================== 成功路径 ====================
    std::printf("\n==================== 成功路径 ====================\n");
    CLines lines;
    const std::shared_ptr<COrderCtx> spCtx = std::make_shared<COrderCtx>();
    spCtx->nQty = 3;
    const CPromise<COrderCtx> pTail = BuildOrderChain(execMain, execDb, billing, spCtx, lines);

    // settled 通知（OnSettled）**不是层**：登记时若还没落定 → 在触发它的那一层的帧里就地执行
    // （此时看得到那一层）；若已落定 → 投递执行（看不到任何层）。两种都合法。
    pTail.OnSettled(
        [](CPromiseResult r)
        {
            std::printf("\n  [通知 OnSettled] 结果=%s\n", r.IsFulfilled() ? "兑现" : "拒绝");
            TraceHere("通知 OnSettled（不是层，不产生新层帧）");
        });
    // 通知也可以指定在哪个执行器上跑（与 ThenOn 对称）。
    pTail.OnSettledOn(execDb,
        [](CPromiseResult)
        {
            std::printf("  [通知 OnSettledOn(execDb)] 这条通知跑在 execDb 上\n");
        });

    const CPromiseResult rOk = pTail.Await();  // 主线程不在层里 → 可以阻塞等
    PrintResult("成功路径", rOk, spCtx);
    ASSERT(rOk.IsFulfilled());
    ASSERT(spCtx->nStock == 5);                 // ③
    ASSERT(spCtx->nDiscount == 30);             // ⑤-1
    ASSERT(spCtx->nTotal == 3 * 100 - 30 - 5);  // ⑤-2 算总额 → ⑪ 减优惠券
    ASSERT(spCtx->nBillNo == 1000);             // ⑥ 跨模块取回账单号
    ASSERT(spCtx->nCoupon == 5);                // ⑨
    ASSERT(spCtx->nGift == 1);                  // ⑩-2a
    ASSERT(spCtx->nPoints == 20);               // ⑩-2b
    ASSERT(spCtx->nLogistics == 1);             // ⑧
    ASSERT(spCtx->nCoroDone.load() == 2);       // ⑩-2 两条并行子链是被协程 CO_AWAIT_ALL 等过的 → 必然都完成
    // 「⑧ 里 Post 出去的旁支也跑完了」这件事**不能直接断言**：那个任务是 fire-and-forget（主链不等它），
    // 框架只保证它最终会跑，不保证跑在主链结束之前 —— 线程越多主链越可能先结束。先等再断言。
    ASSERT(WaitCount(spCtx->nSideDone, 1));
    ASSERT(spCtx->strTrace == std::string("读订单;校验;查库存;读用户;算折扣;算总额;记账;分叉;优惠券;协程券;落库;审计;"));

    // 旁支（⑧）单独等一次，顺便看它那条链。
    const std::shared_ptr<CPromise<COrderCtx> > spSide = spCtx->spSideHold;
    ASSERT(spSide != nullptr);
    ASSERT(spSide->Await().IsFulfilled());

    AssertAuditChain(spCtx, lines);

    //==================== 拒绝路径（同一条父链：库存不足）====================
    std::printf("\n==================== 拒绝路径（同一条父链：库存不足）====================\n");
    CLines linesFail;
    const std::shared_ptr<COrderCtx> spCtxFail = std::make_shared<COrderCtx>();
    spCtxFail->nQty = 3;
    spCtxFail->bFailStock = true;
    const CPromise<COrderCtx> pTailFail = BuildOrderChain(execMain, execDb, billing, spCtxFail, linesFail);
    const CPromiseResult rFail = pTailFail.Await();
    PrintResult("拒绝路径", rFail, spCtxFail);
    ASSERT(rFail.IsRejected());
    ASSERT(rFail.Code() == static_cast<int>(kErrNoStock));                        // ③ 的拒绝码原样透传到调用方
    ASSERT(spCtxFail->nTotal == 0);                                               // ⑤⑨⑪ 都没执行（失败即停）
    ASSERT(spCtxFail->nBillNo == 0);                                              // ⑥ 跨模块也没发起
    ASSERT(spCtxFail->strTrace == std::string("读订单;校验;查库存;补偿;审计;"));  // ⑫ 补偿 + ⑬ 审计执行了

    const std::shared_ptr<CPromise<COrderCtx> > spSideFail = spCtxFail->spSideHold;
    ASSERT(spSideFail != nullptr);
    const CPromiseResult rSideFail = spSideFail->Await();
    // 旁支（⑧）挂在 ⑦ 之下，而主线在 ③ 就失败了 —— 拒绝会沿分叉传播：旁支自己没执行，
    // 但它的句柄同样以这个拒绝码结束（「失败即停」对每一支都成立）。
    ASSERT(rSideFail.IsRejected());
    ASSERT(rSideFail.Code() == static_cast<int>(kErrNoStock));
    ASSERT(spCtxFail->nLogistics == 0);  // ⑧ 确实没跑

#if defined(ASYNC_DEBUG_TRACE)
    // 被跳过的那几层照样在链上 —— 追踪不受「失败即停」影响（它们只是没执行而已）。
    ASSERT(spCtxFail->nAuditChain == spCtx->nAuditChain);                  // 链长和成功路径一样
    ASSERT(HasChainLine(spCtxFail->vecAuditChain, linesFail.nSettle));     // ⑪ 在链上
    ASSERT(HasChainLine(spCtxFail->vecAuditChain, linesFail.nCoroutine));  // ⑩ 在链上
    ASSERT(spCtxFail->strTrace.find("落库;") == std::string::npos);        // 但它确实没执行（轨迹为证）
#endif

    //==================== 其余常用用法 ====================
    DemoCombinators(execMain);

    // AwaitFor：超时兜底（返回 kStopped；不落定、不取消）。
    std::printf("\n==================== Await / AwaitFor ====================\n");
    {
        const std::shared_ptr<COrderCtx> spSlow = std::make_shared<COrderCtx>();
        const CPromiseResult rSlow = MakeQuickChain(execMain, spSlow, false, 0, 200).AwaitFor(20);
        ASSERT(rSlow.IsRejected());
        ASSERT(rSlow.Code() == static_cast<int>(common::async::kStopped));  // 超时 → kStopped
        std::printf("  AwaitFor(20ms) 等一个 200ms 的层 → 拒绝码=%d（kStopped：兜底）\n", rSlow.Code());
    }

    std::printf("\n全部演示通过 ✔（自校验用通用 ASSERT：失败会立即打印位置并中止）\n");

    execMain.Stop();
    execDb.Stop();
    billing.Stop();
    return 0;
}
