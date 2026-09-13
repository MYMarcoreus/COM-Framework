# 示例：一次跨模块下单流程 —— 本框架的五种写法

一条真实的业务流程很少只在一个模块里跑完：**下单服务**要调**库存模块**预占库存、调**支付模块**扣款，
中间还要计价、发货，失败还要**补偿**（释放预占）。

本篇用这一个完整例子（不是玩具片段：模块自持执行器 + 自持上下文、回调式异步接口、跨上下文桥接、
失败补偿、并行调用）展示**同一个流程的几种写法**：正文分五节，每节只讲「怎么把它串起来」；
把各节代码按顺序拼起来就是一个可编译可运行的程序（§6 有实测输出）。

| # | 写法 | 关键 API | 一句话 |
| --- | --- | --- | --- |
| 1 | then 链 + 桥接 | `ThenBridge` | 默认写法：一步一桥，跨模块就是一层 |
| 2 | 失败补偿 | `Catch` + `ThenBridge` | 拒绝后分流，再跨一次模块做**反向操作** |
| 3 | 协程 | `CO_AWAIT` | 直线书写，跨模块直接 await（码透传即终止） |
| 4 | 手写桥接 | `NewPromise(fnStarter)` + `OnSettled` | 不用 `ThenBridge` 时长什么样（它收掉了什么） |
| 5 | 并行调用 | `WhenAll` + `ThenBridge` | 两个模块同时跑，汇聚后继续 |

三条业务路径贯穿全文：**正常**、**库存不足**（第一个桥接就拒绝）、**支付被拒**（需要补偿）。

## 0. 场景与公共部分（只写一次）

```text
下单流程（本模块链）        库存模块（自持 exec "stock"）      支付模块（自持 exec "pay"）
─────────────────         ──────────────────────────       ────────────────────────
建单（金额 = 件数 × 100） ──► 预占库存 ──► 预占号
计价（本模块）              │                           ──► 扣款 ──► 支付号
发货（本模块）◄─────────────┘
（失败）补偿：释放预占 ◄────── 释放预占
```

```cpp
// 公共部分：上下文 / 业务码 / 两个别的模块 / 本模块步骤 —— 五种写法共用
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Coroutine/Coroutine.h"

using common::async::CPromise;
using common::async::CPromiseResult;

/// 业务拒绝码（从 `kBusinessBase` 起取；层间只传码，文案自己查表）。
enum
{
    kStockShortage = common::async::kBusinessBase,
    kPayDeclined
};

/// 拒绝码 → 文案。
static const char* CodeText(int nCode)
{
    switch (nCode)
    {
        case kStockShortage:
            return "库存不足";
        case kPayDeclined:
            return "支付被拒";
        case common::async::kStopped:
            return "执行器已停";
        case common::async::kException:
            return "系统错误";
        default:
            return "未知错误";
    }
}

//================ 库存模块（别的模块：自持执行器 + 自持上下文） ================

/// 库存模块自己的上下文（本模块看不到它的字段）。
struct CStockCtx
{
    std::string strOrderId;
    int nQty;
    int nReserveNo;

    CStockCtx() : nQty(0), nReserveNo(0)
    {}
};

/// 库存模块：预占 / 释放都是它自己的链，在它自己的执行器上跑。
class CStockModule
{
public:
    CStockModule() : m_exec("stock", 1), m_bShortage(false), m_nNextReserveNo(5000)
    {}

    bool Start()
    {
        return m_exec.Start();
    }

    void Stop()
    {
        m_exec.Stop();
    }

    /// 造「库存不足」路径用（真实系统里由库存自己决定）。
    void SetShortage(bool bShortage)
    {
        m_bShortage = bShortage;
    }

    /// 对外异步接口①：预占库存（真实场景是 DB / RPC；这里用「投递 + 回调」模拟对方的完成通知）。
    CPromise<CStockCtx> ReserveAsync(const std::string& strOrderId, int nQty)
    {
        const std::shared_ptr<CStockCtx> spCtx = std::make_shared<CStockCtx>();
        spCtx->strOrderId = strOrderId;
        spCtx->nQty = nQty;

        const int nReserveNo = m_nNextReserveNo++;
        const bool bShortage = m_bShortage;
        const CPromise<CStockCtx>::ChainStarter fnStarter =
            [this, spCtx, nReserveNo, bShortage](
                const CPromise<CStockCtx>::ResolveFn& fnResolve, const CPromise<CStockCtx>::RejectFn& fnReject)
        {
            // 只发起动作 + 登记回调（不阻塞调用方线程）。
            const bool bPosted = m_exec.Post(
                [spCtx, nReserveNo, bShortage, fnResolve, fnReject]()
                {
                    if (bShortage)
                    {
                        fnReject(kStockShortage);  // 业务拒绝：只给码
                        return;
                    }
                    spCtx->nReserveNo = nReserveNo;
                    std::printf("      库存模块: 预占 %d 件（预占号 %d）\n", spCtx->nQty, spCtx->nReserveNo);
                    fnResolve();
                });
            if (!bPosted)
            {
                fnReject(common::async::kStopped);  // 模块已停：别让子链永久挂着
            }
        };
        return m_exec.NewPromise(spCtx, fnStarter, ASYNC_LOC);
    }

    /// 对外异步接口②：释放预占（补偿用）。**幂等**：传 0（没预占过）就是空操作 —— 补偿层不必先判断。
    CPromise<CStockCtx> ReleaseAsync(const std::string& strOrderId, int nReserveNo)
    {
        const std::shared_ptr<CStockCtx> spCtx = std::make_shared<CStockCtx>();
        spCtx->strOrderId = strOrderId;
        spCtx->nReserveNo = nReserveNo;

        const CPromise<CStockCtx>::ChainStarter fnStarter =
            [this, spCtx](const CPromise<CStockCtx>::ResolveFn& fnResolve, const CPromise<CStockCtx>::RejectFn& fnReject)
        {
            const bool bPosted = m_exec.Post(
                [spCtx, fnResolve]()
                {
                    if (spCtx->nReserveNo != 0)
                    {
                        std::printf("      库存模块: 释放预占 %d\n", spCtx->nReserveNo);
                    }
                    fnResolve();
                });
            if (!bPosted)
            {
                fnReject(common::async::kStopped);
            }
        };
        return m_exec.NewPromise(spCtx, fnStarter, ASYNC_LOC);
    }

private:
    common::async::CAsyncExecutor m_exec;  ///< 模块私有执行器（不对外传递）。
    bool m_bShortage;                      ///< 造「库存不足」路径用。
    int m_nNextReserveNo;                  ///< 预占号发生器。
};

//================ 支付模块（另一个别的模块） ================

/// 支付模块自己的上下文。
struct CPayCtx
{
    std::string strOrderId;
    int nAmount;
    int nPayNo;

    CPayCtx() : nAmount(0), nPayNo(0)
    {}
};

/// 支付模块：扣款是它自己的链。
class CPayModule
{
public:
    CPayModule() : m_exec("pay", 1), m_bDecline(false), m_nNextPayNo(9000)
    {}

    bool Start()
    {
        return m_exec.Start();
    }

    void Stop()
    {
        m_exec.Stop();
    }

    /// 造「支付被拒」路径用。
    void SetDecline(bool bDecline)
    {
        m_bDecline = bDecline;
    }

    /// 对外异步接口：扣款。
    CPromise<CPayCtx> ChargeAsync(const std::string& strOrderId, int nAmount)
    {
        const std::shared_ptr<CPayCtx> spCtx = std::make_shared<CPayCtx>();
        spCtx->strOrderId = strOrderId;
        spCtx->nAmount = nAmount;

        const int nPayNo = m_nNextPayNo++;
        const bool bDecline = m_bDecline;
        const CPromise<CPayCtx>::ChainStarter fnStarter =
            [this, spCtx, nPayNo, bDecline](
                const CPromise<CPayCtx>::ResolveFn& fnResolve, const CPromise<CPayCtx>::RejectFn& fnReject)
        {
            const bool bPosted = m_exec.Post(
                [spCtx, nPayNo, bDecline, fnResolve, fnReject]()
                {
                    if (bDecline)
                    {
                        fnReject(kPayDeclined);
                        return;
                    }
                    spCtx->nPayNo = nPayNo;
                    std::printf("      支付模块: 扣款 %d 元（支付号 %d）\n", spCtx->nAmount, spCtx->nPayNo);
                    fnResolve();
                });
            if (!bPosted)
            {
                fnReject(common::async::kStopped);
            }
        };
        return m_exec.NewPromise(spCtx, fnStarter, ASYNC_LOC);
    }

private:
    common::async::CAsyncExecutor m_exec;  ///< 模块私有执行器。
    bool m_bDecline;                       ///< 造「支付被拒」路径用。
    int m_nNextPayNo;                      ///< 支付号发生器。
};

//================ 本模块（下单服务）的上下文与步骤 ================

/// 本模块的上下文：跨模块拿到的数据在 `fnApply` 里搬进这里；层与层之间只传「兑现 / 拒绝」。
struct COrderCtx
{
    std::string strOrderId;  ///< 本模块生成
    int nQty;                ///< 下单件数
    int nAmount;             ///< 计价结果
    int nReserveNo;          ///< 库存模块的预占号（桥接搬回来）
    int nPayNo;              ///< 支付模块的支付号（桥接搬回来）
    std::string strTrace;    ///< 步骤轨迹（自校验）

    /// 要调的别的模块与本模块执行器（真实项目里是接口指针 / ScopedInterfacePtr + 模块自己的执行器）。
    CStockModule* pStock;
    CPayModule* pPay;
    common::async::CAsyncExecutor* pExec;

    /// 并行写法用：两条跨模块调用的句柄（拿到它们才能取对方的上下文）。
    std::shared_ptr<CPromise<CStockCtx> > spReserve;
    std::shared_ptr<CPromise<CPayCtx> > spCharge;

    COrderCtx()
        : nQty(0),
          nAmount(0),
          nReserveNo(0),
          nPayNo(0),
          strTrace(),
          pStock(NULL),
          pPay(NULL),
          pExec(NULL),
          spReserve(),
          spCharge()
    {}
};

/// 步骤 1：建单（链根）——单价 100 元/件，直接算出金额（后续跨模块调用都要用它）。
static CPromiseResult StepCreateOrder(CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
{
    spCtx->strOrderId = "SO-1001";
    spCtx->nAmount = spCtx->nQty * 100;
    spCtx->strTrace += "建单;";
    std::printf("    本模块: 建单 %s（%d 件，%d 元）\n", spCtx->strOrderId.c_str(), spCtx->nQty, spCtx->nAmount);
    return CPromiseResult::Resolve();
}

/// 步骤 2：发货（只有两段跨模块调用都兑现才会跑到这里）。
static CPromiseResult StepShip(CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
{
    spCtx->strTrace += "发货;";
    std::printf("    本模块: 发货（预占号 %d / 支付号 %d）\n", spCtx->nReserveNo, spCtx->nPayNo);
    return CPromiseResult::Resolve();
}

/// 兜底（不恢复）：报告拒绝后原样返回 → 拒绝继续往后传（后续层跳过，`Finally` 仍执行）。
static CPromiseResult StepReportReject(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    spCtx->strTrace += "拒绝;";
    std::printf("    兜底: 流程中断 [%d] %s\n", upResult.Code(), CodeText(upResult.Code()));
    return upResult;
}

/// 收尾审计（finanlly）：三条路径都会跑到。
static CPromiseResult StepAudit(CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
{
    std::printf("    审计: 轨迹=%s\n", spCtx->strTrace.c_str());
    return CPromiseResult::Resolve();
}

//================ 桥接用的四个自由函数（写法 1 / 2 共用） ================

/// `fnCreate`：起对方的链（在**本链线程**上跑，只做「起链 + 登记」）。
static CPromise<CStockCtx> CreateReserve(const std::shared_ptr<COrderCtx>& spSelf)
{
    spSelf->strTrace += "预占;";
    return spSelf->pStock->ReserveAsync(spSelf->strOrderId, spSelf->nQty);
}

/// `fnApply`：把对方上下文的数据搬回本上下文（在**对方线程**上跑 —— 只搬字段，别碰本模块状态）。
static void ApplyReserve(const std::shared_ptr<COrderCtx>& spSelf, const std::shared_ptr<CStockCtx>& spChild)
{
    spSelf->nReserveNo = spChild->nReserveNo;
}

/// `fnCreate`：起支付模块的链。
static CPromise<CPayCtx> CreateCharge(const std::shared_ptr<COrderCtx>& spSelf)
{
    spSelf->strTrace += "扣款;";
    return spSelf->pPay->ChargeAsync(spSelf->strOrderId, spSelf->nAmount);
}

/// `fnApply`：把支付号搬回来。
static void ApplyCharge(const std::shared_ptr<COrderCtx>& spSelf, const std::shared_ptr<CPayCtx>& spChild)
{
    spSelf->nPayNo = spChild->nPayNo;
}
```

几条**所有写法都适用**的规矩：

- 处理器签名固定：`CPromiseResult handler(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)`
  —— 层与层之间只传「兑现 / 拒绝」，数据全放上下文；所以五种写法的步骤都一样。
- `then` 层不用判断 `upResult.IsRejected()`（上游被拒绝时框架直接跳过本层）；只有 `Catch` / `Finally` 需要看它。
- 拒绝只有 `int` 码（业务码从 `kBusinessBase` 起取），文案用 `CodeText` 查表。
- **执行器是模块私有资源**：跨模块只交换 promise + 上下文，不传执行器（上面的 `ReserveAsync` / `ChargeAsync`
  就各自用自己模块的执行器起链）。

## 1. 写法 1：then 链 + `ThenBridge`（默认写法）

一步一桥：轮到桥接层时起对方的链，对方落定后搬数据回本上下文，再继续本链。

```cpp
/// 写法 1：建单 → 〔桥接：预占库存〕 → 〔桥接：扣款〕 → 发货 → 兜底 → 审计。
static void RunBridgeChain(common::async::CAsyncExecutor& exec, CStockModule& stock, CPayModule& pay)
{
    const std::shared_ptr<COrderCtx> spCtx = std::make_shared<COrderCtx>();
    spCtx->nQty = 3;
    spCtx->pStock = &stock;
    spCtx->pPay = &pay;
    spCtx->pExec = &exec;

    exec.NewPromise(spCtx, &StepCreateOrder, ASYNC_LOC)
        .ThenBridge(&CreateReserve, &ApplyReserve, ASYNC_LOC)  // 跨模块①：预占库存
        .ThenBridge(&CreateCharge, &ApplyCharge, ASYNC_LOC)    // 跨模块②：扣款
        .Then(&StepShip, ASYNC_LOC)
        .Catch(&StepReportReject, ASYNC_LOC)
        .Finally(&StepAudit, ASYNC_LOC)
        .Await();
}
```

- **一行一步**：`ThenBridge(fnCreate, fnApply)` = 起子链 + 搬数据，两件事合成一层；
  子链被拒绝 → 本层以**同一拒绝码**被拒绝（后续 `Then` 跳过，`Catch` / `Finally` 照常）。
- 两段跨模块调用是**串行**的：预占成功才扣款。要并行见写法 5。
- `CreateReserve` / `ApplyReserve` 是普通自由函数（也能写成 lambda）；`ApplyReserve` 跑在**对方线程**上，
  所以只搬字段、不碰本模块其他状态。

## 2. 写法 2：失败要补偿（`Catch` 分流 + 反向桥接）

业务上的失败大多要**回滚**：支付被拒 → 释放已预占的库存。写法是把「分流兜底」与「反向操作」也做成链上的层：

```cpp
/// 写法 2 的兜底：按码分流，**返回 Resolve() = 恢复**，让链继续走到补偿层与审计层。
static CPromiseResult StepRecoverByCode(CPromiseResult upResult, const std::shared_ptr<COrderCtx>& spCtx)
{
    spCtx->strTrace += "拒绝;";
    if (upResult.Code() >= common::async::kBusinessBase)
    {
        std::printf("    补偿: 业务拒绝 [%d] %s\n", upResult.Code(), CodeText(upResult.Code()));
    }
    else
    {
        std::printf("    补偿: 系统错误 [%d] %s\n", upResult.Code(), CodeText(upResult.Code()));
    }
    return CPromiseResult::Resolve();  // 恢复：链继续（下面的补偿层 / 审计层照常跑）
}

/// 写法 2 的补偿桥接：**只有真的需要回滚**（有预占、且支付没成功）才释放 ——
/// 释放接口是幂等的：传 0 表示「没有要回滚的东西」，库存模块那边是空操作。
static CPromise<CStockCtx> CreateCompensate(const std::shared_ptr<COrderCtx>& spSelf)
{
    const bool bNeedRollback = (spSelf->nReserveNo != 0 && spSelf->nPayNo == 0);
    if (bNeedRollback)
    {
        spSelf->strTrace += "释放;";
    }
    return spSelf->pStock->ReleaseAsync(spSelf->strOrderId, bNeedRollback ? spSelf->nReserveNo : 0);
}

/// 写法 2：同一条链，但兜底层「恢复」，补偿层再跨一次模块做反向操作。
static void RunCompensatingChain(common::async::CAsyncExecutor& exec, CStockModule& stock, CPayModule& pay)
{
    const std::shared_ptr<COrderCtx> spCtx = std::make_shared<COrderCtx>();
    spCtx->nQty = 3;
    spCtx->pStock = &stock;
    spCtx->pPay = &pay;
    spCtx->pExec = &exec;

    exec.NewPromise(spCtx, &StepCreateOrder, ASYNC_LOC)
        .ThenBridge(&CreateReserve, &ApplyReserve, ASYNC_LOC)
        .ThenBridge(&CreateCharge, &ApplyCharge, ASYNC_LOC)
        .Then(&StepShip, ASYNC_LOC)
        .Catch(&StepRecoverByCode, ASYNC_LOC)                     // 分流 + 恢复
        .ThenBridge(&CreateCompensate, &ApplyReserve, ASYNC_LOC)  // 反向操作（库存模块的释放）
        .Finally(&StepAudit, ASYNC_LOC)
        .Await();
}
```

- 「库存不足」路径：第一个桥接就拒绝 → 兜底报告 → 补偿层的工厂发现「没预占过」→ 给空子链 → 审计。
- 「支付被拒」路径：库存已预占 → 兜底恢复 → 补偿层调库存模块**释放预占**（又跨了一次模块）→ 审计。
- `Catch` 返回 `Resolve()`（恢复）与返回 `upResult`（继续透传）的区别，就是写法 1 与写法 2 的全部差别。

## 3. 写法 3：协程（直线书写，跨模块直接 await）

协程可以 await **任何上下文类型**的 promise：所以跨模块调用就是「一行 await + 把对方的数据搬到本上下文」。

```cpp
/// 写法 3：协程体里顺序书写；跨模块调用直接 await（被等待的 promise 被拒绝 → 协程立即终止，码透传）。
class COrderCoroutine : public common::async::CCoroutine<COrderCtx>
{
public:
    COrderCoroutine(const std::shared_ptr<COrderCtx>& spCtx, CStockModule& stock, CPayModule& pay)
        : common::async::CCoroutine<COrderCtx>(spCtx), m_stock(stock), m_pay(pay), m_pReserve(), m_pCharge()
    {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(StepCreateOrder));  // 本模块的步骤：协程内起一条子 promise 等它

        // 跨模块①：库存模块（别的上下文类型也能 await）
        GetContext()->strTrace += "预占;";
        m_pReserve = std::make_shared<CPromise<CStockCtx> >(m_stock.ReserveAsync(GetContext()->strOrderId, GetContext()->nQty));
        CO_AWAIT(*m_pReserve);                                            // 拒绝 → 协程终止（码透传）
        GetContext()->nReserveNo = m_pReserve->GetContext()->nReserveNo;  // 搬数据（1 行）

        // 跨模块②：支付模块
        GetContext()->strTrace += "扣款;";
        m_pCharge = std::make_shared<CPromise<CPayCtx> >(m_pay.ChargeAsync(GetContext()->strOrderId, GetContext()->nAmount));
        CO_AWAIT(*m_pCharge);
        GetContext()->nPayNo = m_pCharge->GetContext()->nPayNo;

        CO_AWAIT(NewPromise(StepShip));
        CO_RETURN(CPromiseResult::Resolve());
        CO_END();
    }

private:
    CStockModule& m_stock;  ///< 别的模块（引用活着由调用方保证）。
    CPayModule& m_pay;

    /// 跨 await 的变量必须是成员（无栈协程）；`CPromise` 没有默认构造 → 用 shared_ptr 装。
    std::shared_ptr<CPromise<CStockCtx> > m_pReserve;
    std::shared_ptr<CPromise<CPayCtx> > m_pCharge;
};

/// 写法 3：起协程 + 在协程的 promise 上挂兜底与审计。
static void RunCoroutineFlow(common::async::CAsyncExecutor& exec, CStockModule& stock, CPayModule& pay)
{
    const std::shared_ptr<COrderCtx> spCtx = std::make_shared<COrderCtx>();
    spCtx->nQty = 3;
    spCtx->pStock = &stock;
    spCtx->pPay = &pay;
    spCtx->pExec = &exec;

    exec.CoStart<COrderCoroutine>(spCtx, stock, pay)
        ->AsPromise()
        .Catch(&StepReportReject, ASYNC_LOC)
        .Finally(&StepAudit, ASYNC_LOC)
        .Await();
}
```

- `CO_AWAIT(promise)` 非阻塞挂起（不占工作线程）；被 await 的层被拒绝 → 协程立即终止、码透传 →
  交给 `AsPromise().Catch(...)`。
- 跨模块的「搬数据」只有一行（`m_pReserve->GetContext()->...`），因为 `ThenBridge` 的 `fnApply` 在这里就是你手写的。
- 补偿也能写在协程里（`catch` 段之后继续 await 释放），但**拒绝会终止协程**，所以补偿要么放在外层
  `Catch`（写法 2 的形态），要么用 `TryAwait` 风格的显式分支 —— 步骤多、分支多时协程最省心，补偿多时 then 链更直白。

## 4. 写法 4：不用 `ThenBridge` 的手写桥接

`ThenBridge` 到底替你做了什么？不用它就得自己拼三件事：**起一条桥接链** + **在对方落定时搬数据** + **接回本链**。

```cpp
/// 手写桥接：造一条「本上下文」的桥接链 —— 根层由**对方的落定**驱动（先搬数据，再 settle 本链）。
static CPromise<COrderCtx> BridgeReserveManually(common::async::CAsyncExecutor& exec, const std::shared_ptr<COrderCtx>& spCtx)
{
    // ① 起对方的链 —— 必须留住句柄（才能取它的上下文搬数据）；`CPromise` 无默认构造，用 shared_ptr 装。
    spCtx->strTrace += "预占;";
    const std::shared_ptr<CPromise<CStockCtx> > spChild =
        std::make_shared<CPromise<CStockCtx> >(spCtx->pStock->ReserveAsync(spCtx->strOrderId, spCtx->nQty));
    const std::shared_ptr<CStockCtx> spChildCtx = spChild->GetContext();

    // ② 桥接链的根层由外部 settle：对方的通知里搬数据 → resolve / reject
    const CPromise<COrderCtx>::ChainStarter fnStarter =
        [spChild, spChildCtx, spCtx](
            const CPromise<COrderCtx>::ResolveFn& fnResolve, const CPromise<COrderCtx>::RejectFn& fnReject)
    {
        spChild->OnSettled(
            [spChildCtx, spCtx, fnResolve, fnReject](CPromiseResult childResult)
            {
                if (childResult.IsRejected())
                {
                    fnReject(childResult.Code());  // 对方的拒绝码原样透传
                    return;
                }
                spCtx->nReserveNo = spChildCtx->nReserveNo;  // 搬数据（跑在对方线程上：只搬字段）
                fnResolve();
            });
    };

    // ③ 这条链就是「等对方」的桥接链：本链用 ThenPromise 接住它即可
    return exec.NewPromise(spCtx, fnStarter, ASYNC_LOC);
}

/// 写法 4：把写法 1 的两行 `ThenBridge` 换成手写桥接。
static void RunManualBridge(common::async::CAsyncExecutor& exec, CStockModule& stock, CPayModule& pay)
{
    const std::shared_ptr<COrderCtx> spCtx = std::make_shared<COrderCtx>();
    spCtx->nQty = 3;
    spCtx->pStock = &stock;
    spCtx->pPay = &pay;
    spCtx->pExec = &exec;

    const CPromise<COrderCtx>::PromiseFactory fnReserveBridge = [&exec](const std::shared_ptr<COrderCtx>& spSelf)
    {
        return BridgeReserveManually(exec, spSelf);
    };

    exec.NewPromise(spCtx, &StepCreateOrder, ASYNC_LOC)
        .ThenPromise(fnReserveBridge, ASYNC_LOC)             // ← 手写桥接（写法 1 这里是 ThenBridge 一行）
        .ThenBridge(&CreateCharge, &ApplyCharge, ASYNC_LOC)  // 第二段跨模块仍用 ThenBridge（对照）
        .Then(&StepShip, ASYNC_LOC)
        .Catch(&StepReportReject, ASYNC_LOC)
        .Finally(&StepAudit, ASYNC_LOC)
        .Await();
}
```

- 手写版多出来的东西：**留住子 promise 句柄**（`shared_ptr`）、**手动 `OnSettled`**、**手动搬数据**、
  **手动把拒绝码透传**、以及「对方已落定 / 执行器已停」这些边界的收口 —— 这就是 `ThenBridge` 收掉的样板。
- 手写版能做的事 `ThenBridge` 也能做（它内部就是这三件事）；反过来 `ThenBridge` 的 `fnCreate` 只给
  「上下文」，要按运行时条件换子链（如写法 2 的补偿层）得自己判断 —— 两种都留着，**默认用 `ThenBridge`**。

## 5. 写法 5：两个模块并行调用（`WhenAll` 汇聚）

预占库存与扣款互不依赖时可以**并行**发起：先扇出两条跨模块调用，再用 `exec.WhenAll` 汇聚
（聚合只关心成败，所以子 promise 可以跨上下文类型），最后用一个桥接层把两边数据搬回来。

```cpp
/// 扇出层：同时发起两条跨模块调用（各自在自己模块的执行器上跑），句柄记进上下文（后面要取数据）。
static CPromiseResult StepFanOutModules(CPromiseResult /*upResult*/, const std::shared_ptr<COrderCtx>& spCtx)
{
    spCtx->strTrace += "并行;预占;扣款;";  // 两条调用都在这一层发起
    spCtx->spReserve = std::make_shared<CPromise<CStockCtx> >(spCtx->pStock->ReserveAsync(spCtx->strOrderId, spCtx->nQty));
    spCtx->spCharge = std::make_shared<CPromise<CPayCtx> >(spCtx->pPay->ChargeAsync(spCtx->strOrderId, spCtx->nAmount));
    return CPromiseResult::Resolve();
}

/// 汇聚桥接的 `fnCreate`：等两条都落定（`WhenAll`：全部兑现才兑现，任一拒绝立即以该码拒绝）。
static CPromise<COrderCtx> CreateAggregateModules(const std::shared_ptr<COrderCtx>& spSelf)
{
    return spSelf->pExec->WhenAll(spSelf, *spSelf->spReserve, *spSelf->spCharge);
}

/// 汇聚桥接的 `fnApply`：把两边的数据搬回本上下文。
static void ApplyAggregateModules(const std::shared_ptr<COrderCtx>& spSelf, const std::shared_ptr<COrderCtx>& /*spAgg*/)
{
    spSelf->nReserveNo = spSelf->spReserve->GetContext()->nReserveNo;
    spSelf->nPayNo = spSelf->spCharge->GetContext()->nPayNo;
}

/// 写法 5：扇出 → 汇聚 → 发货；任一条被拒 → 汇聚拒绝 → 后续跳过、兜底报告。
static void RunParallelModules(common::async::CAsyncExecutor& exec, CStockModule& stock, CPayModule& pay)
{
    const std::shared_ptr<COrderCtx> spCtx = std::make_shared<COrderCtx>();
    spCtx->nQty = 3;
    spCtx->pStock = &stock;
    spCtx->pPay = &pay;
    spCtx->pExec = &exec;

    exec.NewPromise(spCtx, &StepCreateOrder, ASYNC_LOC)
        .Then(&StepFanOutModules, ASYNC_LOC)                                     // 两条跨模块调用同时发起
        .ThenBridge(&CreateAggregateModules, &ApplyAggregateModules, ASYNC_LOC)  // 等两条都落定 + 搬数据
        .Then(&StepShip, ASYNC_LOC)
        .Catch(&StepReportReject, ASYNC_LOC)
        .Finally(&StepAudit, ASYNC_LOC)
        .Await();
}
```

- 并行只在**两条链之间**（库存模块一条、支付模块一条）；本链本身仍是顺序的（一层跑完跑下一层）。
- `WhenAll` 对齐 JS `Promise.all`：任一拒绝立即失败；要「全部落定再看结果」用 `WhenAllSettled`，
  要「首个落定/首个兑现」用 `WhenRace` / `WhenAny`。
- 注意共享上下文：并行分支只写**不同字段**（这里是各自的子上下文，本上下文只在汇聚层写一次）。

## 6. 一次跑完

把上面各段代码按顺序拼成一个文件（公共部分 + 五种写法 + 下面这个 `main`），三条业务路径各跑一遍。

```cpp
int main()
{
    common::async::CAsyncExecutor exec("order", 2);  // 本模块（下单服务）
    CStockModule stock;                              // 库存模块（自持执行器 "stock"）
    CPayModule pay;                                  // 支付模块（自持执行器 "pay"）
    if (!exec.Start() || !stock.Start() || !pay.Start())
    {
        std::printf("执行器启动失败\n");
        return 1;
    }

    struct CFlow
    {
        const char* pszName;
        void (*fnRun)(common::async::CAsyncExecutor&, CStockModule&, CPayModule&);
    };
    const CFlow aFlows[] = {
        {"1. then 链 + ThenBridge", &RunBridgeChain},
        {"2. 失败补偿（Catch + 反向桥接）", &RunCompensatingChain},
        {"3. 协程（CO_AWAIT 跨模块）", &RunCoroutineFlow},
        {"4. 手写桥接（不用 ThenBridge）", &RunManualBridge},
        {"5. 并行调用两个模块（WhenAll）", &RunParallelModules},
    };

    struct CScenario
    {
        const char* pszName;
        bool bShortage;     // 库存不足
        bool bPayDeclined;  // 支付被拒
    };
    const CScenario aScenarios[] = {
        {"正常", false, false},
        {"库存不足", true, false},
        {"支付被拒", false, true},
    };

    for (size_t i = 0; i < sizeof(aFlows) / sizeof(aFlows[0]); ++i)
    {
        for (size_t j = 0; j < sizeof(aScenarios) / sizeof(aScenarios[0]); ++j)
        {
            stock.SetShortage(aScenarios[j].bShortage);
            pay.SetDecline(aScenarios[j].bPayDeclined);
            std::printf("\n[%s] 场景=%s\n", aFlows[i].pszName, aScenarios[j].pszName);
            aFlows[i].fnRun(exec, stock, pay);
        }
    }

    exec.Stop();
    stock.Stop();
    pay.Stop();
    return 0;
}
```

编译运行（先 `./build.sh --debug Common` 生成 `build/debug/libCommon.a`）：

```bash
g++ -std=c++11 -Wall -Wextra -O0 -g -pthread -ICommon async_bridge.cpp build/debug/libCommon.a -o /tmp/async_bridge
/tmp/async_bridge
```

```text

[1. then 链 + ThenBridge] 场景=正常
    本模块: 建单 SO-1001（3 件，300 元）
      库存模块: 预占 3 件（预占号 5000）
      支付模块: 扣款 300 元（支付号 9000）
    本模块: 发货（预占号 5000 / 支付号 9000）
    审计: 轨迹=建单;预占;扣款;发货;

[1. then 链 + ThenBridge] 场景=库存不足
    本模块: 建单 SO-1001（3 件，300 元）
    兜底: 流程中断 [100] 库存不足
    审计: 轨迹=建单;预占;拒绝;

[1. then 链 + ThenBridge] 场景=支付被拒
    本模块: 建单 SO-1001（3 件，300 元）
      库存模块: 预占 3 件（预占号 5002）
    兜底: 流程中断 [101] 支付被拒
    审计: 轨迹=建单;预占;扣款;拒绝;

[2. 失败补偿（Catch + 反向桥接）] 场景=正常
    本模块: 建单 SO-1001（3 件，300 元）
      库存模块: 预占 3 件（预占号 5003）
      支付模块: 扣款 300 元（支付号 9002）
    本模块: 发货（预占号 5003 / 支付号 9002）
    审计: 轨迹=建单;预占;扣款;发货;

[2. 失败补偿（Catch + 反向桥接）] 场景=库存不足
    本模块: 建单 SO-1001（3 件，300 元）
    补偿: 业务拒绝 [100] 库存不足
    审计: 轨迹=建单;预占;拒绝;

[2. 失败补偿（Catch + 反向桥接）] 场景=支付被拒
    本模块: 建单 SO-1001（3 件，300 元）
      库存模块: 预占 3 件（预占号 5005）
    补偿: 业务拒绝 [101] 支付被拒
      库存模块: 释放预占 5005
    审计: 轨迹=建单;预占;扣款;拒绝;释放;

[3. 协程（CO_AWAIT 跨模块）] 场景=正常
    本模块: 建单 SO-1001（3 件，300 元）
      库存模块: 预占 3 件（预占号 5006）
      支付模块: 扣款 300 元（支付号 9004）
    本模块: 发货（预占号 5006 / 支付号 9004）
    审计: 轨迹=建单;预占;扣款;发货;

[3. 协程（CO_AWAIT 跨模块）] 场景=库存不足
    本模块: 建单 SO-1001（3 件，300 元）
    兜底: 流程中断 [100] 库存不足
    审计: 轨迹=建单;预占;拒绝;

[3. 协程（CO_AWAIT 跨模块）] 场景=支付被拒
    本模块: 建单 SO-1001（3 件，300 元）
      库存模块: 预占 3 件（预占号 5008）
    兜底: 流程中断 [101] 支付被拒
    审计: 轨迹=建单;预占;扣款;拒绝;

[4. 手写桥接（不用 ThenBridge）] 场景=正常
    本模块: 建单 SO-1001（3 件，300 元）
      库存模块: 预占 3 件（预占号 5009）
      支付模块: 扣款 300 元（支付号 9006）
    本模块: 发货（预占号 5009 / 支付号 9006）
    审计: 轨迹=建单;预占;扣款;发货;

[4. 手写桥接（不用 ThenBridge）] 场景=库存不足
    本模块: 建单 SO-1001（3 件，300 元）
    兜底: 流程中断 [100] 库存不足
    审计: 轨迹=建单;预占;拒绝;

[4. 手写桥接（不用 ThenBridge）] 场景=支付被拒
    本模块: 建单 SO-1001（3 件，300 元）
      库存模块: 预占 3 件（预占号 5011）
    兜底: 流程中断 [101] 支付被拒
    审计: 轨迹=建单;预占;扣款;拒绝;

[5. 并行调用两个模块（WhenAll）] 场景=正常
    本模块: 建单 SO-1001（3 件，300 元）
      库存模块: 预占 3 件（预占号 5012）
      支付模块: 扣款 300 元（支付号 9008）
    本模块: 发货（预占号 5012 / 支付号 9008）
    审计: 轨迹=建单;并行;预占;扣款;发货;

[5. 并行调用两个模块（WhenAll）] 场景=库存不足
    本模块: 建单 SO-1001（3 件，300 元）
      支付模块: 扣款 300 元（支付号 9009）
    兜底: 流程中断 [100] 库存不足
    审计: 轨迹=建单;并行;预占;扣款;拒绝;

[5. 并行调用两个模块（WhenAll）] 场景=支付被拒
    本模块: 建单 SO-1001（3 件，300 元）
      库存模块: 预占 3 件（预占号 5014）
    兜底: 流程中断 [101] 支付被拒
    审计: 轨迹=建单;并行;预占;扣款;拒绝;
```

- 三条路径的**轨迹**（`审计:` 那行）在五种写法里逐字一致 —— 差别只在被拒路径上「怎么收尾」：
  写法 1 报告后透传、写法 2 分流并回滚（多一个 `释放;`）、协程直接终止、写法 4/5 同写法 1。
- **写法 5 的「库存不足」路径**：库存模块拒绝的同时，支付模块那条链**照样跑完**（并行分支互不取消，
  参考 JS `Promise.all` 的语义：聚合立即失败，但不撤销其它分支）—— 真实项目里要留意这类「已经发生的副作用」。
- 写法 5 的并行场景里，`库存模块:` / `支付模块:` 两行来自**两个模块线程**，先后不定
  （上面的样例取自一次实际运行）；本框架不保证它们的打印顺序 —— 这也正是「并行」的样子。
- 预占号 / 支付号每次递增，所以同一场景在不同写法里编号不同，不影响对照。


## 7. 五种写法对照表与判据

| | 1. then 链 + 桥接 | 2. 失败补偿 | 3. 协程 | 4. 手写桥接 | 5. 并行调用 |
| --- | --- | --- | --- | --- | --- |
| 跨模块怎么写 | `ThenBridge(fnCreate, fnApply)` | 同左 + 反向再桥一次 | `CO_AWAIT(子promise)` + `GetContext()` 搬数据 | `NewPromise(fnStarter)` + `OnSettled` + `ThenPromise` | 扇出 `WhenAll` + `ThenBridge` 汇聚 |
| 本层跑在哪条线程 | 本链执行器（跨模块返回自动拉回） | 同左 | 协程自己的执行器 | 同左 | 同左 |
| 对方的拒绝怎么处理 | 同码拒绝本层（后续跳过） | `Catch` 分流后可**恢复** | 协程立即终止（码透传） | 手动 `fnReject(childResult.Code())` | 聚合立即拒绝 |
| 失败要做反向操作 | 用 `Catch` + 再 `ThenBridge`（写法 2） | 就是本法 | 放外层 `Catch`，或显式分支 | 同写法 1 | 同写法 1 |
| 代码量 | 最少 | 少（多一个分流 + 一个补偿工厂） | 中（跨 await 变量要变成员） | 多（自己收口边界） | 中（多扇出一层） |
| 什么时候用 | **默认** | 有跨模块副作用要回滚 | 步骤多 / 分支循环多 | 只为了理解 `ThenBridge` 内部 | 两个模块调用互不依赖、要压低时延 |

判据（从上到下问自己）：

1. **是不是跨模块**（上下文类型不同）？是 → `ThenBridge`；不是 → `Then` / `ThenPromise`。
2. **失败要不要回滚**？要 → `Catch` 分流并返回 `Resolve()` 恢复，再补一个「反向操作」的桥接层（写法 2）。
3. **几个模块调用之间有依赖吗**？没有 → 扇出 + `WhenAll` 汇聚（写法 5）。
4. **步骤是不是又多又带分支/循环**？是 → 用协程直线写（写法 3），末尾统一 `Catch` + `Finally`。
5. 需要「无论成败都收尾」→ `Finally`（不改结果）；把拒绝码透传给上层 → `Catch` 里返回 `upResult`。

相关：跨模块与执行器归属的细则见 [async-usage.md](async-usage.md) §6.3 / §8，
机制与边界（桥接层的结算线程、通知不迁移等）见 [async-impl.md](async-impl.md) §6.1 / §7。
