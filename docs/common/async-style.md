# 示例：同一个业务流，五种写法

同一个业务流（`fetchUser → fetchOrders → checkRisk → fetchPayment → 总额`），
用**五种风格**串起来摆在一起看：它们只差「怎么把这几步接起来」，业务代码一个字都不用改。

| # | 写法 | 临摹的生态 | 一句话 |
| --- | --- | --- | --- |
| 1 | then 链 | JavaScript Promise | `then` 串链 + 内层链（`ThenPromise`）+ `catch` 兜底 |
| 2 | 协程 | libgo | 一个协程里直线书写（`CO_AWAIT`），被拒绝即中断 |
| 3 | 手动 settle | async_promise | `make_promise(resolve, reject)` 显式兑现 / 拒绝 |
| 4 | 拒绝后恢复 | Async++ | `recover` 里按码分流兜底，返回 `Resolve()` 让链继续 |
| 5 | 跨模块 / 跨上下文 | 本框架特有（JS / libgo 没有对应物） | `ThenBridge`：起子链 + 搬数据，跨出本模块 |

业务流有两条路径：**正常**（有订单 → 通过风控 → 支付 → 打印总额）与**风控拒绝**
（没有订单 → `Reject(kNoOrders)` → 后续步骤全跳过）。五种写法里，两条路径都跑一遍；写法 1–3 的输出完全一致（见 §6）。

## 0. 业务流与公共部分（只写一次）

五种写法**共用**这份代码：上下文 `CFlowCtx`、业务码、文案表、A–E 五个步骤处理器
（§5 的跨模块写法另外加了一个「支付模块」和它自己的上下文）。
差别只在下一节开始出现的「怎么串起来」。

```cpp
// 公共部分：上下文 / 业务码 / 步骤处理器 —— 五种写法共用
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Coroutine/Coroutine.h"

using common::async::CPromise;
using common::async::CPromiseResult;

/// 上下文：等价各种写法里沿流程流动的值（user / orders / order / payment / total）。
struct CFlowCtx
{
    int nUserId;                         ///< 入参：fetchUser(123)
    bool bNoOrders;                      ///< 造「风控拒绝」路径用（测试开关）
    std::string strUserName;             ///< A 产出
    std::vector<std::string> vecOrders;  ///< B 产出
    std::string strOrderId;              ///< C 产出
    int nTotal;                          ///< D 产出

    CFlowCtx() : nUserId(0), bNoOrders(false), nTotal(0)
    {}
};

/// 业务拒绝码（从 `kBusinessBase` 起取；等价各种写法里的 reject reason.code）。
enum
{
    kNoOrders = common::async::kBusinessBase,
    kTooMany
};

/// 拒绝码 → 文案（层间只传码，文案自己查表；等价 reason.msg）。
static const char* CodeText(int nCode)
{
    switch (nCode)
    {
        case kNoOrders:
            return "没有订单可处理";
        case kTooMany:
            return "订单过多，需人工审核";
        case common::async::kException:
            return "系统错误";
        default:
            return "未知错误";
    }
}

/// 步骤 A：fetchUser(123)
static CPromiseResult StepFetchUser(CPromiseResult /*upResult*/, const std::shared_ptr<CFlowCtx>& spCtx)
{
    spCtx->strUserName = "Alice";
    std::printf("用户: %s\n", spCtx->strUserName.c_str());
    return CPromiseResult::Resolve();
}

/// 步骤 B：fetchOrders(user)
static CPromiseResult StepFetchOrders(CPromiseResult /*upResult*/, const std::shared_ptr<CFlowCtx>& spCtx)
{
    if (!spCtx->bNoOrders)
    {
        spCtx->vecOrders.push_back("order1");
        spCtx->vecOrders.push_back("order2");
    }
    std::printf("订单数: %zu\n", spCtx->vecOrders.size());
    return CPromiseResult::Resolve();
}

/// 步骤 C：checkRisk(orders) —— 条件不满足就拒绝（等价 `reject(...)` / `throw RejectReason`）
static CPromiseResult StepCheckRisk(CPromiseResult /*upResult*/, const std::shared_ptr<CFlowCtx>& spCtx)
{
    if (spCtx->vecOrders.empty())
    {
        return CPromiseResult::Reject(kNoOrders);
    }
    if (spCtx->vecOrders.size() > 10)
    {
        return CPromiseResult::Reject(kTooMany);
    }
    spCtx->strOrderId = spCtx->vecOrders.front();
    std::printf("通过风控，订单: %s\n", spCtx->strOrderId.c_str());
    return CPromiseResult::Resolve();
}

/// 步骤 D：fetchPayment(order) —— 只有步骤 C 兑现才会执行
static CPromiseResult StepFetchPayment(CPromiseResult /*upResult*/, const std::shared_ptr<CFlowCtx>& spCtx)
{
    spCtx->nTotal = 99;
    std::printf("支付: %d\n", spCtx->nTotal);
    return CPromiseResult::Resolve();
}

/// 步骤 E：打印最终总额（等价 `.then(total => …)`）
static CPromiseResult StepPrintTotal(CPromiseResult /*upResult*/, const std::shared_ptr<CFlowCtx>& spCtx)
{
    std::printf("最终总额: %d\n", spCtx->nTotal);
    return CPromiseResult::Resolve();
}

/// 统一兜底（写法 1 / 2 / 3 共用）：只在被拒绝时执行；返回原 `upResult` = 没吞掉，继续往外传。
static CPromiseResult OnReject(CPromiseResult upResult, const std::shared_ptr<CFlowCtx>& /*spCtx*/)
{
    std::printf("流程中断 [%d]: %s\n", upResult.Code(), CodeText(upResult.Code()));
    return upResult;
}
```

几条**所有写法都适用**的规矩：

- **处理器签名固定**：`CPromiseResult handler(CPromiseResult upResult, const std::shared_ptr<CFlowCtx>& spCtx)`
  —— 层与层之间只传「兑现 / 拒绝」，数据全放上下文；所以五种写法的步骤 A–E 完全一样。
- **`then` 层不用判断 `upResult.IsRejected()`**：上游被拒绝时框架直接跳过本层（失败即停）；
  只有 `Catch` / `Finally` 需要看它。
- **拒绝只剩 `int` 码**：业务码从 `kBusinessBase` 起取，文案用 `CodeText` 查表。

## 1. 写法 1：then 链（JavaScript Promise 风格）

JS 的写法：外层 `then` 里 `return` 内层链；`checkRisk` 不满足时 `return Promise.reject(reason)`，
链上后续 `then` 全部跳过，最后 `.catch(...)` 统一兜底。

```js
fetchUser(123)
  .then(user => {
    return fetchOrders(user)              // 内层链
      .then(orders => checkRisk(orders))  // 条件不满足时返回 rejected promise
      .then(order => fetchPayment(order))
      .then(payment => payment.total);
  })
  .then(total => console.log('最终总额:', total))
  .catch(reason => console.error(`流程中断 [${reason.code}]: ${reason.msg}`));
```

本框架：`.then(回调)` → `Then(处理器)`、`.then(user => 内层链)` → `ThenPromise(工厂)`、
`.catch(...)` → `Catch(处理器)`。

```cpp
/// 等价 `fetchUser(123).then(user => 内层链).then(total => …).catch(reason => …)`
static void RunThenChain(common::async::CAsyncExecutor& exec, bool bFail)
{
    const std::shared_ptr<CFlowCtx> spCtx = std::make_shared<CFlowCtx>();
    spCtx->nUserId = 123;
    spCtx->bNoOrders = bFail;

    // `.then(user => { return 内层链 })`：内层链 = fetchOrders → checkRisk → fetchPayment → total
    const CPromise<CFlowCtx>::PromiseFactory fnInner = [&exec](const std::shared_ptr<CFlowCtx>& spSelf)
    {
        return exec.NewPromise(spSelf, &StepFetchOrders, ASYNC_LOC)
            .Then(&StepCheckRisk, ASYNC_LOC)
            .Then(&StepFetchPayment, ASYNC_LOC)
            .Then(&StepPrintTotal, ASYNC_LOC);
    };

    exec.NewPromise(spCtx, &StepFetchUser, ASYNC_LOC).ThenPromise(fnInner, ASYNC_LOC).Catch(&OnReject, ASYNC_LOC).Await();
}
```

- `.then(回调)` → `Then(处理器)`；每层的数据是上下文，不是沿链流动的值。
- `.then(user => { return 内层链 })` → **`ThenPromise(工厂)`**：外层等内层链落定才继续
  （JS 里「回调返回 promise 会被展平」是自动的，本框架要显式写这一种层）。
- `return Promise.reject(reason)` → 处理器返回 `Reject(码)`：**拒绝即停**，后面的 `Then` 全跳过。
- `.catch(reason => …)` → `Catch(处理器)`：返回原 `upResult` 表示没吞掉；返回 `Resolve()` 即恢复（见写法 4）。
- `reason` 可以是任意类型 → 本框架只有 `int` 码（文案自建表）。

## 2. 写法 2：协程（libgo 风格）

libgo 的写法：一个协程里逐步 `co_await` 同步书写，业务拒绝抛异常中断，`catch` 统一兜底。

```text
void businessFlow()
{
    try
    {
        User user = fetchUser(123);          // 函数体内部 co_await
        auto orders = fetchOrders(user);
        Order order = checkRisk(orders);     // 不满足则 throw RejectReason
        Payment payment = fetchPayment(order);
    }
    catch (const RejectReason& r)
    { /* 业务拒绝 */
    }
    catch (const std::exception& ex)
    { /* 真实异常 */
    }
}
```

本框架：`CCoroutine` + `CO_AWAIT(NewPromise(步骤))` 直线书写；**被 await 的步骤被拒绝 → 协程立即终止**
（后面的 `CO_AWAIT` 不再执行），`AsPromise().Catch(...)` 就是那个 `catch`。

```cpp
/// 等价 `void businessFlow()`：一个协程里顺序书写，每步 CO_AWAIT。
class CFlowCoroutine : public common::async::CCoroutine<CFlowCtx>
{
public:
    using common::async::CCoroutine<CFlowCtx>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(StepFetchUser));     // User user = fetchUser(123);
        CO_AWAIT(NewPromise(StepFetchOrders));   // auto orders = fetchOrders(user);
        CO_AWAIT(NewPromise(StepCheckRisk));     // Order order = checkRisk(orders);（拒绝 → 终止）
        CO_AWAIT(NewPromise(StepFetchPayment));  // Payment payment = fetchPayment(order);
        CO_AWAIT(NewPromise(StepPrintTotal));    // std::cout << "最终总额: " << …;
        CO_RETURN(CPromiseResult::Resolve());
        CO_END();
    }
};

/// 等价 `go businessFlow; co_sched.RunUntilNoTask();`
static void RunCoroutine(common::async::CAsyncExecutor& exec, bool bFail)
{
    const std::shared_ptr<CFlowCtx> spCtx = std::make_shared<CFlowCtx>();
    spCtx->nUserId = 123;
    spCtx->bNoOrders = bFail;

    exec.CoStart<CFlowCoroutine>(spCtx)->AsPromise().Catch(&OnReject, ASYNC_LOC).Await();
}
```

- `co_await xxx` → `CO_AWAIT(NewPromise(步骤))`：**非阻塞挂起**（不占 worker）。
- 局部变量 `user` / `orders` / `order` / `payment` → 上下文字段；协程与它起的子 promise **共用同一个上下文实例**。
- `throw RejectReason{code, msg}` → 步骤返回 `Reject(码)`；被 await 的层被拒绝 → 协程立即终止。
- `try / catch` → 协程 `AsPromise().Catch(处理器)`（返回原 `upResult` = 没吞掉）。
- `go businessFlow` + `RunUntilNoTask()` → `exec.CoStart<CFlowCoroutine>(spCtx)` + `Await()` 等它跑完。

## 3. 写法 3：手动 settle（async_promise 风格）

async_promise 的写法：`make_promise([](resolve, reject) { … })` —— 起链回调**同步执行**，
里面发起动作并登记回调，最后由回调 `resolve(...)` / `reject(...)` 兑现或拒绝。

```text
auto checkRiskAsync = make_promise([&](auto resolve, auto reject)
{
    riskEngine.check(orders, [=](const RiskResult& r)
    {
        if (!r.ok)
            reject(RejectReason{"RISK", r.msg});
        else
            resolve(r.order);
    });
});
```

本框架：`exec.NewPromise(spCtx, fnStarter)`（起链回调拿到 `ResolveFn` / `RejectFn`），
再把这条 promise 用 `ThenPromise` 接回主链。

```cpp
/// checkRiskAsync：等价 `make_promise([](resolve, reject) { … })` —— 显式兑现 / 拒绝本 promise。
static CPromise<CFlowCtx> CheckRiskAsync(common::async::CAsyncExecutor& exec, const std::shared_ptr<CFlowCtx>& spCtx)
{
    const CPromise<CFlowCtx>::ChainStarter fnStarter =
        [spCtx](const CPromise<CFlowCtx>::ResolveFn& fnResolve, const CPromise<CFlowCtx>::RejectFn& fnReject)
    {
        if (spCtx->vecOrders.empty())
        {
            fnReject(kNoOrders);  // 等价 reject(RejectReason{"NO_ORDERS", …})
            return;
        }
        if (spCtx->vecOrders.size() > 10)
        {
            fnReject(kTooMany);
            return;
        }
        spCtx->strOrderId = spCtx->vecOrders.front();
        std::printf("通过风控，订单: %s\n", spCtx->strOrderId.c_str());
        fnResolve();  // 等价 resolve(orders[0])
    };

    return exec.NewPromise(spCtx, fnStarter, ASYNC_LOC);
}

/// 等价 `fetchUserAsync(id).then(…).then(…).fail(...)`
static void RunManualSettle(common::async::CAsyncExecutor& exec, bool bFail)
{
    const std::shared_ptr<CFlowCtx> spCtx = std::make_shared<CFlowCtx>();
    spCtx->nUserId = 123;
    spCtx->bNoOrders = bFail;

    // checkRisk 那条子 promise 接回主链（等价 then 回调里 return promise）
    const CPromise<CFlowCtx>::PromiseFactory fnCheckRisk = [&exec](const std::shared_ptr<CFlowCtx>& spSelf)
    {
        return CheckRiskAsync(exec, spSelf);
    };

    exec.NewPromise(spCtx, &StepFetchUser, ASYNC_LOC)
        .Then(&StepFetchOrders, ASYNC_LOC)
        .ThenPromise(fnCheckRisk, ASYNC_LOC)
        .Then(&StepFetchPayment, ASYNC_LOC)
        .Then(&StepPrintTotal, ASYNC_LOC)
        .Catch(&OnReject, ASYNC_LOC)
        .Await();
}
```

- `make_promise(回调)` → `exec.NewPromise(spCtx, fnStarter)`；起链回调**同步执行**，不占 worker。
- `reject(RejectReason{code, msg})` → `fnReject(码)`；`resolve(value)` → `fnResolve()`（值写进上下文字段）。
- 子 promise 接回主链用 **`ThenPromise(工厂)`**：外层等它落定，拒绝码原样透传。
- `.fail(...)` → `.Catch(处理器)`。

## 4. 写法 4：拒绝后恢复（Async++ 风格）

Async++ 的写法：拒绝经 `.recover(...)` 兜底，**兜底之后链还能继续**往下走（这正是「恢复」与「终止」的区别）。

```text
spawn([&] { /* fetchUser → checkRisk … 用 tce.set_value / tce.set_exception 报告结果 */ })
    .then(...)
    .recover([&](const std::exception_ptr& e) { /* 按类型分流兜底 */ })
    .then([&] { /* 两条路径都会走到这里 */ });
```

本框架：`.recover(...)` → `.Catch(处理器)`，处理器里**按码分流**（业务码 / 框架码），
返回 `Resolve()` 即恢复 —— 后面的 `Then` 照常执行。

```cpp
/// `.recover(...)`：按码分流（业务码 / 框架码）并**恢复**（返回 Resolve 让链继续）。
static CPromiseResult StepRecover(CPromiseResult upResult, const std::shared_ptr<CFlowCtx>& /*spCtx*/)
{
    if (upResult.Code() >= common::async::kBusinessBase)
    {
        std::printf("流程中断 [%d]: %s\n", upResult.Code(), CodeText(upResult.Code()));  // 业务拒绝
    }
    else
    {
        std::printf("系统错误 [%d]: %s\n", upResult.Code(), CodeText(upResult.Code()));  // 层内异常等
    }
    return CPromiseResult::Resolve();  // 恢复：等价 recover 之后链还能继续往下走
}

/// recover 之后的续接层：两条路径都会走到这里
static CPromiseResult StepDone(CPromiseResult /*upResult*/, const std::shared_ptr<CFlowCtx>& /*spCtx*/)
{
    std::printf("流程结束: 资源已回收\n");
    return CPromiseResult::Resolve();
}

/// 等价 `spawn(…).then(…).then(…).recover(…)`
static void RunRecover(common::async::CAsyncExecutor& exec, bool bFail)
{
    const std::shared_ptr<CFlowCtx> spCtx = std::make_shared<CFlowCtx>();
    spCtx->nUserId = 123;
    spCtx->bNoOrders = bFail;

    exec.NewPromise(spCtx, &StepFetchUser, ASYNC_LOC)
        .Then(&StepFetchOrders, ASYNC_LOC)
        .Then(&StepCheckRisk, ASYNC_LOC)
        .Then(&StepFetchPayment, ASYNC_LOC)
        .Then(&StepPrintTotal, ASYNC_LOC)
        .Catch(&StepRecover, ASYNC_LOC)
        .Then(&StepDone, ASYNC_LOC)
        .Await();
}
```

- `.recover(...)` → `.Catch(处理器)`：返回 `Resolve()` = **恢复**（后续 `Then` 照常跑）；
  返回原 `upResult` = 继续往外传（写法 1 的 `OnReject` 就是这种）。
- 「两个 `catch` 子句」（`RejectReason` / `std::exception`）→ **按码分流**：
  `码 >= kBusinessBase` 是业务拒绝，`kException` / `kRejected` / `kStopped` 是系统侧。
- `tce.set_value(...)` / `tce.set_exception(...)` → 处理器返回 `Resolve()` / `Reject(码)`，
  或用 `exec.NewPromise(spCtx, fnStarter)` 手动兑现 / 拒绝（即写法 3）。
- `chain.get()` / `.wait()` → `.Await()`。

## 5. 写法 5：跨模块 / 跨上下文（`ThenBridge`）

前四种写法都在**一个模块内部**：一条链、一份上下文。真实项目总会有一步是「调别的模块」——
对方自持执行器、上下文类型也是它自己的，这时用 **`ThenBridge(fnCreate, fnApply)`**：
轮到这一层时起子链（`fnCreate`），子链落定后把数据搬回本上下文（`fnApply`），再继续本链。

同一个业务流，只把「支付」一步交给**支付模块**：

```cpp
/// 支付模块的上下文（别的模块自己的数据，本链看不到）。
struct CPayCtx
{
    std::string strOrderId;
    int nAmount;
    int nPayNo;

    CPayCtx() : nAmount(0), nPayNo(0)
    {}
};

/// 支付模块：**自持执行器** + 自持上下文（跨模块只交换 promise + 上下文，不传执行器）。
class CPayModule
{
public:
    explicit CPayModule(const char* pszName) : m_exec(pszName, 1)
    {}

    bool Start()
    {
        return m_exec.Start();
    }

    void Stop()
    {
        m_exec.Stop();
    }

    /// 对外异步接口：把订单信息接进来，返回**自己的** promise（调用方用 ThenBridge 等它）。
    CPromise<CPayCtx> PayAsync(const std::string& strOrderId, int nAmount)
    {
        const std::shared_ptr<CPayCtx> spPayCtx = std::make_shared<CPayCtx>();
        spPayCtx->strOrderId = strOrderId;
        spPayCtx->nAmount = nAmount;
        return m_exec.NewPromise(spPayCtx, &StepPay, ASYNC_LOC);
    }

private:
    /// 模块自己的链：真正干活的层（示例里支付额就是总价）。
    static CPromiseResult StepPay(CPromiseResult /*upResult*/, const std::shared_ptr<CPayCtx>& spPayCtx)
    {
        spPayCtx->nPayNo = 20250913;
        std::printf("支付: %d（支付模块，单号 %d）\n", spPayCtx->nAmount, spPayCtx->nPayNo);
        return CPromiseResult::Resolve();
    }

    common::async::CAsyncExecutor m_exec;  ///< 模块私有执行器。
};

/// 等价「调支付模块 → 搬数据 → 继续本链」：ThenBridge = 这两件事合成一层。
static void RunBridge(common::async::CAsyncExecutor& exec, bool bFail)
{
    const std::shared_ptr<CFlowCtx> spCtx = std::make_shared<CFlowCtx>();
    spCtx->nUserId = 123;
    spCtx->bNoOrders = bFail;

    CPayModule pay("pay");  // 别的模块（自己的执行器 + 自己的上下文）
    pay.Start();

    /// 起子链：在**本链执行器线程**上执行，只做「起链 + 登记回调」，别做重活。
    const std::function<CPromise<CPayCtx>(const std::shared_ptr<CFlowCtx>&)> fnCreatePay =
        [&pay](const std::shared_ptr<CFlowCtx>& spSelf)
    {
        return pay.PayAsync(spSelf->strOrderId, 99);
    };

    /// 搬数据：跑在**子链的结算线程**上（对方模块的线程）→ 只搬字段，别碰本模块其他状态。
    const std::function<void(const std::shared_ptr<CFlowCtx>&, const std::shared_ptr<CPayCtx>&)> fnApplyPay =
        [](const std::shared_ptr<CFlowCtx>& spSelf, const std::shared_ptr<CPayCtx>& spPayCtx)
    {
        spSelf->nTotal = spPayCtx->nAmount;  // 子上下文 → 本上下文
    };

    exec.NewPromise(spCtx, &StepFetchUser, ASYNC_LOC)
        .Then(&StepFetchOrders, ASYNC_LOC)
        .Then(&StepCheckRisk, ASYNC_LOC)
        .ThenBridge(fnCreatePay, fnApplyPay, ASYNC_LOC)  // ← 跨模块 / 跨上下文就这一行
        .Then(&StepPrintTotal, ASYNC_LOC)
        .Catch(&OnReject, ASYNC_LOC)
        .Await();

    pay.Stop();
}
```

不写 `ThenBridge` 的话，这一层要自己拼三件事（`ThenBridge` 收掉的就是这份样板）：

```text
① exec.NewPromise(spCtx, fnStarter)                  // 造一条「由外部 settle」的桥接链
   └─ 在对方的 OnSettled 回调里：搬数据 → fnResolve() / fnReject(码)
② p = p.ThenPromise(fnWaitBridge)                    // 把它接进本链（本链等它落定）
③ 子链被拒绝 → 拒绝码原样透传（后续 Then 跳过、Catch / Finally 照常）
```

- **同上下文**（同一条链内的子链）用 `ThenPromise` 就够（写法 1）；**跨上下文 / 跨模块**才需要 `ThenBridge`。
- `fnCreate` 在本链线程上跑（只起链）；`fnApply` 在**子链的结算线程**上跑（对方的线程）→ 只搬数据。
- 子链被拒绝 → 本层以**同一拒绝码**被拒绝：本例「风控拒绝」路径下支付模块压根不会被调到。
- 跑完这一层之后，后续层自动回**本链执行器**线程（不会一直待在对方线程上）。

## 6. 五种写法一起跑

把上面各段代码按顺序拼成一个文件（公共部分 + 五种写法 + 下面这个 `main`），就能一次跑完、对照输出。

```cpp
int main()
{
    common::async::CAsyncExecutor exec("main", 2);
    exec.Start();

    struct CStyle
    {
        const char* pszName;
        void (*fnRun)(common::async::CAsyncExecutor&, bool);
    };
    const CStyle aStyles[] = {
        {"1. then 链（JS Promise 风格）", &RunThenChain},
        {"2. 协程（libgo 风格）", &RunCoroutine},
        {"3. 手动 settle（async_promise 风格）", &RunManualSettle},
        {"4. 拒绝后恢复（Async++ 风格）", &RunRecover},
        {"5. 跨模块 / 跨上下文（ThenBridge）", &RunBridge},
    };

    for (size_t i = 0; i < sizeof(aStyles) / sizeof(aStyles[0]); ++i)
    {
        for (int nPath = 0; nPath < 2; ++nPath)  // 正常 / 风控拒绝 两条路径
        {
            std::printf("\n[%s] 路径=%s\n", aStyles[i].pszName, (nPath == 0) ? "正常" : "风控拒绝");
            aStyles[i].fnRun(exec, nPath != 0);
        }
    }

    exec.Stop();
    return 0;
}
```

编译运行（先 `./build.sh --debug Common` 生成 `build/debug/libCommon.a`）：

```bash
g++ -std=c++11 -Wall -Wextra -O0 -g -pthread -ICommon async_style.cpp build/debug/libCommon.a -o /tmp/async_style
/tmp/async_style
```

```text

[1. then 链（JS Promise 风格）] 路径=正常
用户: Alice
订单数: 2
通过风控，订单: order1
支付: 99
最终总额: 99

[1. then 链（JS Promise 风格）] 路径=风控拒绝
用户: Alice
订单数: 0
流程中断 [100]: 没有订单可处理

[2. 协程（libgo 风格）] 路径=正常
用户: Alice
订单数: 2
通过风控，订单: order1
支付: 99
最终总额: 99

[2. 协程（libgo 风格）] 路径=风控拒绝
用户: Alice
订单数: 0
流程中断 [100]: 没有订单可处理

[3. 手动 settle（async_promise 风格）] 路径=正常
用户: Alice
订单数: 2
通过风控，订单: order1
支付: 99
最终总额: 99

[3. 手动 settle（async_promise 风格）] 路径=风控拒绝
用户: Alice
订单数: 0
流程中断 [100]: 没有订单可处理

[4. 拒绝后恢复（Async++ 风格）] 路径=正常
用户: Alice
订单数: 2
通过风控，订单: order1
支付: 99
最终总额: 99
流程结束: 资源已回收

[4. 拒绝后恢复（Async++ 风格）] 路径=风控拒绝
用户: Alice
订单数: 0
流程中断 [100]: 没有订单可处理
流程结束: 资源已回收

[5. 跨模块 / 跨上下文（ThenBridge）] 路径=正常
用户: Alice
订单数: 2
通过风控，订单: order1
支付: 99（支付模块，单号 20250913）
最终总额: 99

[5. 跨模块 / 跨上下文（ThenBridge）] 路径=风控拒绝
用户: Alice
订单数: 0
流程中断 [100]: 没有订单可处理
```

写法 1 / 2 / 3 在两条路径上**输出完全一致**；写法 4 多一行「流程结束: 资源已回收」
（它的 `Catch` 返回 `Resolve()`，链继续走到了 `StepDone`）；写法 5 的支付行来自支付模块（多带一个单号）。

## 7. 五种写法怎么选

| | 1. then 链 | 2. 协程 | 3. 手动 settle | 4. 拒绝后恢复 | 5. 跨模块（`ThenBridge`） |
| --- | --- | --- | --- | --- | --- |
| 关键 API | `Then` / `ThenPromise` / `Catch` | `CoStart` + `CO_AWAIT` | `NewPromise(spCtx, fnStarter)` | `Catch` 返回 `Resolve()` | `ThenBridge(fnCreate, fnApply)` |
| 拒绝怎么表达 | 处理器返回 `Reject(码)` | 被 await 的层返回 `Reject(码)` | `fnReject(码)` | 同写法 1 | 子链被拒绝 → 本层同码拒绝 |
| 拒绝后谁接管 | 后面的 `Then` 全跳过 → `Catch` | 协程立即终止 → `AsPromise().Catch` | 同写法 1 | `Catch` 里分流 → **链继续** | 同写法 1 |
| 要写几层 | 每步一层（嵌套时用 `ThenPromise`） | 一个协程体写完 | 只有「回调式异步」那一步要单独写 | 同写法 1 | 一层搞定（起子链 + 搬数据） |
| 上下文 | 本模块的 | 本模块的 | 本模块的 | 本模块的 | **别的模块的**（`fnApply` 搬回来） |
| 适合 | 步骤少、要跟 JS 心智对齐 | 步骤多 / 有分支与循环 | 对接回调式 API（网络库、第三方 SDK） | 失败可降级、收尾必须执行 | 那一步要调**别的模块**（对方自持执行器） |

- 混着用完全可以：**主流程用协程直线写**，其中「对接回调式 API」的那一步用 `NewPromise(spCtx, fnStarter)`
  （写法 3）包一层，「调别的模块」的那一步用 `ThenBridge`（写法 5），末尾统一 `Catch`（写法 1 / 4）。
- 判据先看**上下文**：同一条链里加一步 → `Then` / `ThenPromise`；跨模块（上下文类型不同）→ `ThenBridge`。
- 需要「无论成败都收尾」时用 `Finally`（不改结果）；需要「失败降级后继续」时用写法 4。
- 别把拒绝当成异常逃逸：层内抛异常会被框架转成 `kException`（写法 4 的按码分流正好能区分两者）。

相关：机制与边界见 [async-impl.md](async-impl.md)，选型与 API 全貌见 [async-usage.md](async-usage.md)，
与 JS 的逐项对照见 [async-vs-js.md](async-vs-js.md)。
