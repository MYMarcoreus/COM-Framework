// ====================================================================
// 例子：一条 promise 链里混用多种 then（下单流程）
// （精简版见 docs/common/async-mixed-then-example.md）
//
// 三个模块各一个类，各自持有自己的执行器（执行器不跨模块传递）：
//   COrderModule    下单流程：本文件的重点，链在 PlaceOrderAsync() 里组装
//   CStockModule    库存模块：对外只有 QueryStockAsync()
//   CBillingModule  记账模块：对外只有 WriteBillingAsync()（旁支，主链不等它）
//
// 链（编号与实现它的函数一一对应）：
//   ① StepLoad             具名异步函数
//   ② StepValidate         校验（不需要捕获，写成静态成员函数）
//   ③ fnQueryStock         ThenPromise：调库存模块并等它（跨上下文，BridgeQueryStock 桥接）
//   ④ fnReserveInner       ThenPromise：现搭内层链并等它（同上下文，直接 adopt）
//   ⑤ fnBillingSideBranch  普通 Then 里起链但不等它（fire-and-forget 旁支）
//   ⑥ StepSaveOrder        具名异步函数
//   catch StepCompensate / finally StepAudit
//
// 四条路径都跑一遍（自校验）：
//   正常下单   → 全链兑现（库存 5 件，买 3 件 → 打折）
//   库存不足   → ③ 的桥接拒绝，④…⑥ 不执行；catch 补偿后仍透传拒绝
//   参数非法   → ② 拒绝，后面全不执行（连库存模块都没被调用）
//   内层链拒绝 → ④ 的内层链拒绝，拒绝码沿外层链透传（⑤⑥ 不执行，catch/finally 仍执行）
//
// 排版约定：lambda 先赋给具名变量再串链；每行一个 then、注释放行尾、≤ 120 列。
// ====================================================================
#include "cases/ThenMixCase.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"

namespace no = common::async;

namespace {

// ==================== 三套上下文：本流程 / 库存模块 / 记账模块
// ====================

/// @brief 下单流程的共享上下文（本模块的 TContext）。
struct COrderContext
{
    int nSku;           ///< 商品编码。
    int nQty;           ///< 购买数量。
    int nUnitPrice;     ///< 单价（分）。
    int nTotal;         ///< 应付合计（分）。
    int nStock;         ///< 库存模块返回的可售量。
    bool bStockEnough;  ///< 库存是否充足（桥接层写）。
    bool bAudited;      ///< 是否已审计（finally 层写）。
    bool bCompensated;  ///< 是否已补偿（catch 层写）。
    bool bFailReserve;  ///< 演示开关：内层链「确认预占」强制拒绝（用来看拒绝如何透传）。
    std::atomic<int> nBillingDone;  ///< 记账旁支是否完成（旁支晚于主链，用原子避免竞争）。
    std::string strTrace;           ///< 执行轨迹。

    COrderContext()
        : nSku(0),
          nQty(0),
          nUnitPrice(0),
          nTotal(0),
          nStock(0),
          bStockEnough(false),
          bAudited(false),
          bCompensated(false),
          bFailReserve(false),
          nBillingDone(0)
    {}
};

/// @brief 库存模块的上下文（**另一套类型**：库存服务只认自己的数据）。
struct CStockContext
{
    int nSku;              ///< 商品编码。
    int nAvail;            ///< 可售量。
    std::string strTrace;  ///< 库存模块自己的轨迹。

    CStockContext() : nSku(0), nAvail(0)
    {}
};

/// @brief 记账模块的上下文（旁支用，**独立实例** →
/// 与主链不共享字段、无并发写竞争）。
struct CBillingContext
{
    int nOrderId;          ///< 订单号。
    int nAmount;           ///< 记账金额（分）。
    std::string strTrace;  ///< 记账模块自己的轨迹。

    CBillingContext() : nOrderId(0), nAmount(0)
    {}
};

// ==================== 短别名（上下文定好后立刻起别名，后面到处都用它）
// ====================

using COrderPromise = no::CPromise<COrderContext>;      ///< 下单流程的 promise。
using CStockPromise = no::CPromise<CStockContext>;      ///< 库存模块的 promise。
using CBillingPromise = no::CPromise<CBillingContext>;  ///< 记账模块的 promise。

/// 本用例的业务错误码（业务码从 kBusinessBase 起取）。
enum ThenMixCode
{
    kCodeOutOfStock = no::kBusinessBase + 1,    ///< 库存不足。
    kCodeBadOrder = no::kBusinessBase + 2,      ///< 订单参数非法（单笔最多 10 件）。
    kCodeReserveFailed = no::kBusinessBase + 3  ///< 内层链：确认预占失败。
};

/// @brief 断言助手：失败时打印原因，返回本条的通过状态。
///
/// @param bCond 断言条件。
/// @param strWhat 断言说明。
/// @param strDetail 失败时的补充信息。
///
/// @return bCond。
bool Expect(bool bCond, const std::string& strWhat, const std::string& strDetail)
{
    if (!bCond)
    {
        std::printf("  [ASSERT] 失败: %s (%s)\n", strWhat.c_str(), strDetail.c_str());
    }
    return bCond;
}

// ====================================================================
// 库存模块：自持执行器，对外只给 promise
// ====================================================================

/// @brief 库存模块（演示用的最小模块）：执行器随模块生灭，对外只有异步函数。
class CStockModule
{
   public:
    CStockModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 异步查可售量：在模块自己的执行器上起链（内部两步，调用方不关心）。
    ///
    /// @param nSku 商品编码。
    ///
    /// @return 库存模块自己上下文的 promise（调用方只能等它，拿不到执行器）。
    CStockPromise QueryStockAsync(int nSku)
    {
        std::shared_ptr<CStockContext> spStock = std::make_shared<CStockContext>();
        spStock->nSku = nSku;
        return m_exec.NewPromise(spStock, &StepConnect, ASYNC_LOC).Then(&StepRead, ASYNC_LOC);
    }

   private:
    /// 层 1：建连（模拟握手）。
    static no::CPromiseResult StepConnect(no::CPromiseResult /*upResult*/, const std::shared_ptr<CStockContext>& spCtx)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        spCtx->strTrace += "连库存;";
        return no::CPromiseResult::Resolve();
    }

    /// 层 2：读可售量。
    static no::CPromiseResult StepRead(no::CPromiseResult /*upResult*/, const std::shared_ptr<CStockContext>& spCtx)
    {
        spCtx->nAvail = 5;  // 演示数据：只有 5 件可售
        spCtx->strTrace += "读库存;";
        return no::CPromiseResult::Resolve();
    }

    no::CAsyncExecutor m_exec;  ///< 模块私有执行器（析构自动 Stop）。
};

// ====================================================================
// 记账模块：同样自持执行器（旁支用，主链不等它）
// ====================================================================

/// @brief 记账模块（演示用的最小模块）：执行器随模块生灭，对外只有异步函数。
class CBillingModule
{
   public:
    CBillingModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 异步写记账流水：在模块自己的执行器上起链。
    ///
    /// @param nOrderId 订单号。
    /// @param nAmount 金额（分）。
    ///
    /// @return 记账模块自己上下文的 promise。
    CBillingPromise WriteBillingAsync(int nOrderId, int nAmount)
    {
        std::shared_ptr<CBillingContext> spBill = std::make_shared<CBillingContext>();
        spBill->nOrderId = nOrderId;
        spBill->nAmount = nAmount;
        return m_exec.NewPromise(spBill, &StepWrite, ASYNC_LOC);
    }

   private:
    /// 唯一一步：写流水（模拟 IO）。
    static no::CPromiseResult StepWrite(no::CPromiseResult /*upResult*/, const std::shared_ptr<CBillingContext>& spCtx)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        spCtx->strTrace += "写流水;";
        return no::CPromiseResult::Resolve();
    }

    no::CAsyncExecutor m_exec;  ///< 模块私有执行器（析构自动 Stop）。
};

// ====================================================================
// 下单模块：同样自持执行器；链在 PlaceOrderAsync() 里组装
// ====================================================================

/// @brief 下单模块（演示用的最小模块）：链里的 lambda 捕获 this，模块要比链活得久。
class COrderModule
{
   public:
    COrderModule() : m_exec(2)
    {
        m_exec.Start();
    }

    /// @brief 异步下单：① ~ ⑥ + catch + finally 串成一条链。
    ///
    /// @param spCtx 下单流程上下文（入参：nSku / nQty）。
    /// @param spStockModule 库存模块（自持执行器；调用方只能拿它的 promise）。
    /// @param spBillingModule 记账模块（自持执行器；旁支用）。
    ///
    /// @return 指向 finally 层的 promise 句柄。
    COrderPromise PlaceOrderAsync(const std::shared_ptr<COrderContext>& spCtx,
                                  const std::shared_ptr<CStockModule>& spStockModule,
                                  const std::shared_ptr<CBillingModule>& spBillingModule)
    {
        // ③ 工厂：then 内部执行其他模块的异步函数，并等它（跨上下文 → 桥接）。
        COrderPromise::PromiseFactory fnQueryStock =
            [this, spStockModule](const std::shared_ptr<COrderContext>& spCtxSelf)
        {
            return BridgeQueryStock(spCtxSelf, spStockModule);
        };

        // ④ 工厂：在 lambda 里现搭一条内层 then 链，让它参与当前链（同上下文 → 直接 adopt）。
        COrderPromise::PromiseFactory fnReserveInner = [this](const std::shared_ptr<COrderContext>& spCtxSelf)
        {
            return m_exec.NewPromise(spCtxSelf, &StepReserveStock, ASYNC_LOC).Then(&StepReserveConfirm, ASYNC_LOC);
        };

        // ⑤ 旁支：then 内部执行其他异步函数，但不等它（fire-and-forget）。
        COrderPromise::ThenHandler fnBillingSideBranch =
            [spBillingModule](no::CPromiseResult /*upResult*/, const std::shared_ptr<COrderContext>& spCtxSelf)
        {
            const int nOrderId = spCtxSelf->nSku * 1000 + spCtxSelf->nQty;
            CBillingPromise promiseBill = spBillingModule->WriteBillingAsync(nOrderId, spCtxSelf->nTotal);
            // 旁支的收尾通知跑在记账模块的线程上 → 只写原子，不做重活。
            promiseBill.OnSettled([spCtxSelf](no::CPromiseResult result)
            {
                if (result.IsFulfilled())
                {
                    spCtxSelf->nBillingDone.store(1, std::memory_order_relaxed);
                }
            });
            spCtxSelf->strTrace += "记账已发起(不等);";
            return no::CPromiseResult::Resolve();  // 主链继续，不等待记账。
        };

        return m_exec
            .NewPromise(spCtx, &StepLoad, ASYNC_LOC)  // ① 具名异步函数
            .Then(&StepValidate, ASYNC_LOC)           // ② 校验
            .ThenPromise(fnQueryStock, ASYNC_LOC)     // ③ 调库存模块（等它）
            .Then(&StepApplyDiscount, ASYNC_LOC)      // ④ 算折扣
            .ThenPromise(fnReserveInner, ASYNC_LOC)   // ④+ 内层链（等它）
            .Then(fnBillingSideBranch, ASYNC_LOC)     // ⑤ 记账旁支（不等它）
            .Then(&StepSaveOrder, ASYNC_LOC)          // ⑥ 落库
            .Catch(&StepCompensate, ASYNC_LOC)        // catch：仅被拒绝时执行
            .Finally(&StepAudit, ASYNC_LOC);          // finally：成败都跑
    }

   private:
    /// ① 读订单：取单价（模拟一次 IO）。
    static no::CPromiseResult StepLoad(no::CPromiseResult /*upResult*/, const std::shared_ptr<COrderContext>& spCtx)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        spCtx->nUnitPrice = 1250;  // 12.50 元
        spCtx->strTrace += "读订单;";
        return no::CPromiseResult::Resolve();
    }

    /// ② 校验（不需要捕获，写成静态成员函数）。
    static no::CPromiseResult StepValidate(no::CPromiseResult /*upResult*/, const std::shared_ptr<COrderContext>& spCtx)
    {
        if (spCtx->nSku <= 0 || spCtx->nQty <= 0)
        {
            spCtx->strTrace += "校验失败(参数非法);";
            return no::CPromiseResult::Reject(kCodeBadOrder);
        }
        if (spCtx->nQty > 10)
        {
            spCtx->strTrace += "校验失败(单笔最多 10 件);";
            return no::CPromiseResult::Reject(kCodeBadOrder);
        }
        spCtx->strTrace += "校验通过;";
        return no::CPromiseResult::Resolve();
    }

    /// ③ 桥接：把库存模块的 promise 接进本流程（等价 JS 的 new Promise）。
    ///
    /// 桥接层属于本模块，用本模块的 m_exec 创建；库存模块在自己的执行器上跑。
    ///
    /// @param spCtx 下单流程上下文。
    /// @param spStockModule 库存模块。
    ///
    /// @return 由库存模块的回调 settle 的本流程 promise。
    COrderPromise BridgeQueryStock(const std::shared_ptr<COrderContext>& spCtx,
                                   const std::shared_ptr<CStockModule>& spStockModule)
    {
        COrderPromise::PromiseExecutor fnExecutor =
            [spStockModule, spCtx](const COrderPromise::ResolveFn& fnResolve, const COrderPromise::RejectFn& fnReject)
        {
            // 发起跨模块调用：拿到的是库存模块上下文的 promise，执行器在模块内部。
            CStockPromise promiseStock = spStockModule->QueryStockAsync(spCtx->nSku);
            const std::shared_ptr<CStockContext> spStock = promiseStock.GetContext();
            promiseStock.OnSettled([spCtx, spStock, fnResolve, fnReject](no::CPromiseResult result)
            {
                // 本回调在库存模块的线程上：只做语义转换 + 改上下文 + settle。
                if (result.IsRejected())
                {
                    fnReject(result.Code());  // 库存模块拒绝 → 本流程拒绝（原样透传）。
                    return;
                }
                spCtx->nStock = spStock->nAvail;
                spCtx->bStockEnough = (spStock->nAvail >= spCtx->nQty);
                spCtx->strTrace += "查库存(" + std::to_string(spStock->nAvail) + ");";
                if (!spCtx->bStockEnough)
                {
                    spCtx->strTrace += "库存不足;";
                    fnReject(kCodeOutOfStock);  // 业务拒绝：后续 then 不执行。
                    return;
                }
                fnResolve();
            });
        };
        return COrderPromise::New(m_exec, spCtx, fnExecutor, ASYNC_LOC);
    }

    /// ④ 算折扣：满 3 件 9 折（模拟业务规则）。
    static no::CPromiseResult StepApplyDiscount(no::CPromiseResult /*upResult*/,
                                                const std::shared_ptr<COrderContext>& spCtx)
    {
        spCtx->nTotal = spCtx->nUnitPrice * spCtx->nQty;
        if (spCtx->nQty >= 3)
        {
            spCtx->nTotal = spCtx->nTotal * 9 / 10;  // 满 3 件 9 折
            spCtx->strTrace += "满减;";
        }
        else
        {
            spCtx->strTrace += "无折扣;";
        }
        return no::CPromiseResult::Resolve();
    }

    /// ④+ 内层链第 1 步：预占库存。
    static no::CPromiseResult StepReserveStock(no::CPromiseResult /*upResult*/,
                                               const std::shared_ptr<COrderContext>& spCtx)
    {
        spCtx->strTrace += "预占;";
        return no::CPromiseResult::Resolve();
    }

    /// ④+ 内层链第 2 步：确认预占（bFailReserve 时拒绝，用来看拒绝如何透传）。
    static no::CPromiseResult StepReserveConfirm(no::CPromiseResult /*upResult*/,
                                                 const std::shared_ptr<COrderContext>& spCtx)
    {
        if (spCtx->bFailReserve)
        {
            return no::CPromiseResult::Reject(kCodeReserveFailed);
        }
        spCtx->strTrace += "确认预占;";
        return no::CPromiseResult::Resolve();
    }

    /// ⑥ 落库：保存订单（模拟一次写库）。
    static no::CPromiseResult StepSaveOrder(no::CPromiseResult /*upResult*/,
                                            const std::shared_ptr<COrderContext>& spCtx)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        spCtx->strTrace += "落库;";
        return no::CPromiseResult::Resolve();
    }

    /// catch 补偿：库存不足时补偿（只在上一层被拒绝时执行）。
    ///
    /// 这里透传拒绝（return upResult）；若补偿后可以继续，返回 Resolve() 吞掉拒绝。
    static no::CPromiseResult StepCompensate(no::CPromiseResult upResult, const std::shared_ptr<COrderContext>& spCtx)
    {
        if (upResult.Code() == kCodeOutOfStock)  // 只有库存不足需要释放 / 回滚
        {
            spCtx->bCompensated = true;
            spCtx->strTrace += "补偿;";
        }
        return upResult;
    }

    /// finally 审计：成败都执行，返回值被忽略（原样透传上一层结果）。
    static no::CPromiseResult StepAudit(no::CPromiseResult upResult, const std::shared_ptr<COrderContext>& spCtx)
    {
        spCtx->bAudited = true;
        spCtx->strTrace += "审计;";
        return upResult;
    }

    no::CAsyncExecutor m_exec;  ///< 模块私有执行器（析构自动 Stop）。
};

/// @brief 等旁支（记账）完成：旁支是 fire-and-forget，断言前等一下。
///
/// 真实业务里不需要等 —— 旁支有自己的收尾（写统计 /
/// 记日志）；这里只是为了让用例可断言。
///
/// @param spCtx 下单流程上下文。
/// @param nTimeoutMs 最长等待毫秒数。
static void WaitSideBranch(const std::shared_ptr<COrderContext>& spCtx, int nTimeoutMs)
{
    const int nStepMs = 2;
    for (int nWaited = 0; nWaited < nTimeoutMs && spCtx->nBillingDone.load(std::memory_order_relaxed) == 0;
         nWaited += nStepMs)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(nStepMs));
    }
}

}  // namespace

/// @brief 运行「混用多种 then」的下单流程用例。
///
/// @return true 全部自校验通过；false 有断言失败。
bool RunThenMixCase()
{
    auto spOrderModule = std::make_shared<COrderModule>();      // 下单模块（自持执行器）
    auto spStockModule = std::make_shared<CStockModule>();      // 库存模块（自持执行器）
    auto spBillingModule = std::make_shared<CBillingModule>();  // 记账模块（自持执行器）
    bool bOk = true;

    // ---------------- 路径 ①：正常下单（6 种 then + catch + finally 全跑一遍）
    // ----------------
    std::shared_ptr<COrderContext> spOk = std::make_shared<COrderContext>();
    spOk->nSku = 88;
    spOk->nQty = 3;  // 满 3 件 → ④ 打折

    const no::CPromiseResult resultOk = spOrderModule->PlaceOrderAsync(spOk, spStockModule, spBillingModule).Await();
    WaitSideBranch(spOk, 1000);  // ⑤ 是旁支：主链不等它，这里等一下再断言

    const bool bOkStock = (spOk->nStock == 5 && spOk->bStockEnough);
    const bool bOkTrace = (spOk->strTrace ==
                           "读订单;校验通过;查库存(5);满减;预占;确认预占;"
                           "记账已发起(不等);落库;审计;");
    bOk = Expect(resultOk.IsFulfilled(), "正常下单：最终兑现", "码=" + std::to_string(resultOk.Code())) && bOk;
    bOk = Expect(spOk->nTotal == 3375, "正常下单：合计=1250×3×0.9=3375", "实际=" + std::to_string(spOk->nTotal)) && bOk;
    bOk = Expect(bOkStock, "正常下单：库存 5 件足够", "库存=" + std::to_string(spOk->nStock)) && bOk;
    bOk = Expect(spOk->nBillingDone.load() == 1, "正常下单：旁支记账已完成", "nBillingDone!=1") && bOk;
    bOk = Expect(spOk->bAudited, "正常下单：finally 审计已执行", "bAudited=false") && bOk;
    bOk = Expect(bOkTrace, "正常下单：执行顺序符合预期", "轨迹=" + spOk->strTrace) && bOk;
    std::printf("㉘ 混用多种 then（正常下单）: 合计=%d 库存=%d 轨迹=%s\n", spOk->nTotal, spOk->nStock,
                spOk->strTrace.c_str());

    // ---------------- 路径 ②：库存不足（③ 的桥接拒绝 → ④⑤⑥ 不执行，catch
    // 补偿后仍透传） ----------------
    std::shared_ptr<COrderContext> spOut = std::make_shared<COrderContext>();
    spOut->nSku = 88;
    spOut->nQty = 8;  // 库存只有 5 件 → ③ 查库存后拒绝

    const no::CPromiseResult resultOut = spOrderModule->PlaceOrderAsync(spOut, spStockModule, spBillingModule).Await();

    const bool bOkOutCode = (resultOut.IsRejected() && resultOut.Code() == static_cast<int>(kCodeOutOfStock));
    const bool bOkOutTrace = (spOut->strTrace == "读订单;校验通过;查库存(5);库存不足;补偿;审计;");
    bOk = Expect(bOkOutCode, "库存不足：拒绝码为 kCodeOutOfStock", "码=" + std::to_string(resultOut.Code())) && bOk;
    bOk = Expect(spOut->bCompensated, "库存不足：catch 补偿已执行", "bCompensated=false") && bOk;
    bOk = Expect(spOut->nTotal == 0, "库存不足：④ 算折扣未执行", "nTotal=" + std::to_string(spOut->nTotal)) && bOk;
    bOk = Expect(bOkOutTrace, "库存不足：执行顺序符合预期", "轨迹=" + spOut->strTrace) && bOk;
    std::printf("㉘ 混用多种 then（库存不足）: 拒绝码=%d 轨迹=%s\n", resultOut.Code(), spOut->strTrace.c_str());

    // ---------------- 路径 ③：参数非法（② 的 lambda 拒绝 →
    // 连库存模块都没被调用） ----------------
    std::shared_ptr<COrderContext> spBad = std::make_shared<COrderContext>();
    spBad->nSku = 88;
    spBad->nQty = 50;  // 单笔最多 10 件 → ② 直接拒绝

    const no::CPromiseResult resultBad = spOrderModule->PlaceOrderAsync(spBad, spStockModule, spBillingModule).Await();

    const bool bOkBadCode = (resultBad.IsRejected() && resultBad.Code() == static_cast<int>(kCodeBadOrder));
    const bool bOkBadTrace = (spBad->strTrace == "读订单;校验失败(单笔最多 10 件);审计;");
    bOk = Expect(bOkBadCode, "参数非法：拒绝码为 kCodeBadOrder", "码=" + std::to_string(resultBad.Code())) && bOk;
    bOk = Expect(bOkBadTrace, "参数非法：失败即停（未查库存、未记账）", "轨迹=" + spBad->strTrace) && bOk;
    bOk = Expect(spBad->nBillingDone.load() == 0, "参数非法：旁支未被发起", "nBillingDone!=0") && bOk;
    std::printf("㉘ 混用多种 then（参数非法）: 拒绝码=%d 轨迹=%s\n", resultBad.Code(), spBad->strTrace.c_str());

    // ---------------- 路径 ④：内层链拒绝 → 拒绝码沿外层链透传（后续 then
    // 不执行，catch/finally 仍执行）
    // ----------------
    std::shared_ptr<COrderContext> spInner = std::make_shared<COrderContext>();
    spInner->nSku = 88;
    spInner->nQty = 2;             // 不触发折扣，也不缺库存
    spInner->bFailReserve = true;  // 内层链第 2 步（确认预占）拒绝

    const no::CPromiseResult resultInner =
        spOrderModule->PlaceOrderAsync(spInner, spStockModule, spBillingModule).Await();

    const bool bOkInnerCode = (resultInner.IsRejected() && resultInner.Code() == static_cast<int>(kCodeReserveFailed));
    const bool bOkInnerTrace = (spInner->strTrace == "读订单;校验通过;查库存(5);无折扣;预占;审计;");
    bOk = Expect(bOkInnerCode, "内层链拒绝：拒绝码透传到外层", "码=" + std::to_string(resultInner.Code())) && bOk;
    bOk = Expect(bOkInnerTrace, "内层链拒绝：外层后续 then 不执行、finally 仍执行", "轨迹=" + spInner->strTrace) && bOk;
    bOk = Expect(spInner->nBillingDone.load() == 0, "内层链拒绝：后续旁支未发起", "nBillingDone!=0") && bOk;
    std::printf("㉘ 内层链参与当前链（内层拒绝）: 拒绝码=%d 轨迹=%s\n", resultInner.Code(), spInner->strTrace.c_str());

    // 模块的执行器随模块析构自动停（真实项目里由框架按依赖顺序启停）。
    return bOk;
}
