# 示例：Async++ 风格（recover 统一兜底，恢复后链继续）

Async++ 的写法：条件不满足时用 `task_completion_event` 手动 `set_exception(...)`（不 throw）；
链尾 `.recover(...)` 统一兜底 —— 里面按异常类型分流，`recover` 之后的链还能继续往下走。

```cpp
async::task<Order> checkRiskAsync(const std::vector<Order>& orders)
{
    async::task_completion_event<Order> tce;
    if (orders.empty())
    {
        tce.set_exception(std::make_exception_ptr(RejectReason{"NO_ORDERS", "没有订单可处理"}));
    }
    else
    {
        tce.set_value(orders[0]);
    }
    return tce.get_task();
}
// ...
.recover([](std::exception_ptr e)
{
    try
    {
        std::rethrow_exception(e);
    }
    catch (const RejectReason& r)
    { /* 业务拒绝 */
    }
    catch (const std::exception& ex)
    { /* 真实异常 */
    }
});
```

本框架写法（可编译运行）：

```cpp
// 对标 Async++：Catch 统一兜底 —— 按码分流业务拒绝 / 系统异常，恢复后链继续。
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"

/// 上下文：等价这些写法里沿链流动的值（user / orders / order / payment / total）。
struct CFlowCtx
{
    int nUserId;
    bool bNoOrders;
    std::string strUserName;
    std::vector<std::string> vecOrders;
    std::string strOrderId;
    int nTotal;

    CFlowCtx() : nUserId(0), bNoOrders(false), nTotal(0)
    {}
};

/// 业务拒绝码（从 kBusinessBase 起取；等价 RejectReason.code）。
enum
{
    kNoOrders = common::async::kBusinessBase,
    kTooMany
};

/// 拒绝码 → 文案（层间只传码，文案自己查表；等价 RejectReason.msg）。
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

/// fetchUserAsync(id)
static common::async::CPromiseResult StepFetchUser(common::async::CPromiseResult /*upResult*/,
                                                   const std::shared_ptr<CFlowCtx>& spCtx)
{
    spCtx->strUserName = "Alice";
    std::printf("用户: %s\n", spCtx->strUserName.c_str());
    return common::async::CPromiseResult::Resolve();
}

/// fetchOrdersAsync(user)
static common::async::CPromiseResult StepFetchOrders(common::async::CPromiseResult /*upResult*/,
                                                     const std::shared_ptr<CFlowCtx>& spCtx)
{
    if (!spCtx->bNoOrders)
    {
        spCtx->vecOrders.push_back("order1");
        spCtx->vecOrders.push_back("order2");
    }
    std::printf("订单数: %zu\n", spCtx->vecOrders.size());
    return common::async::CPromiseResult::Resolve();
}

/// checkRiskAsync：条件不满足时 Reject（等价 tce.set_exception(...)，不 throw）
static common::async::CPromiseResult StepCheckRisk(common::async::CPromiseResult /*upResult*/,
                                                   const std::shared_ptr<CFlowCtx>& spCtx)
{
    if (spCtx->vecOrders.empty())
    {
        return common::async::CPromiseResult::Reject(kNoOrders);
    }
    if (spCtx->vecOrders.size() > 10)
    {
        return common::async::CPromiseResult::Reject(kTooMany);
    }
    spCtx->strOrderId = spCtx->vecOrders.front();
    std::printf("通过风控，订单: %s\n", spCtx->strOrderId.c_str());
    return common::async::CPromiseResult::Resolve();
}

/// fetchPaymentAsync(order) —— 只有 checkRisk 兑现才会执行
static common::async::CPromiseResult StepFetchPayment(common::async::CPromiseResult /*upResult*/,
                                                      const std::shared_ptr<CFlowCtx>& spCtx)
{
    spCtx->nTotal = 99;
    std::printf("支付: %d\n", spCtx->nTotal);
    return common::async::CPromiseResult::Resolve();
}

/// `.then(total => …)`
static common::async::CPromiseResult StepPrintTotal(common::async::CPromiseResult /*upResult*/,
                                                    const std::shared_ptr<CFlowCtx>& spCtx)
{
    std::printf("最终总额: %d\n", spCtx->nTotal);
    return common::async::CPromiseResult::Resolve();
}

/// `.recover(...)`：统一兜底 —— 按码分流（业务码 / 框架码），并「恢复」让链继续
static common::async::CPromiseResult StepRecover(common::async::CPromiseResult upResult,
                                                 const std::shared_ptr<CFlowCtx>& /*spCtx*/)
{
    if (upResult.Code() >= common::async::kBusinessBase)
    {
        std::fprintf(stderr, "流程中断 [%d]: %s\n", upResult.Code(), CodeText(upResult.Code()));  // 业务拒绝
    }
    else
    {
        std::fprintf(stderr, "系统错误 [%d]: %s\n", upResult.Code(), CodeText(upResult.Code()));  // 层内异常等
    }
    return common::async::CPromiseResult::Resolve();  // 恢复：等价 recover 之后链还能继续往下走
}

/// recover 之后的续接层：两种路径都会走到这里
static common::async::CPromiseResult StepDone(common::async::CPromiseResult /*upResult*/,
                                              const std::shared_ptr<CFlowCtx>& /*spCtx*/)
{
    std::printf("流程结束: 资源已回收\n");
    return common::async::CPromiseResult::Resolve();
}

int main()
{
    common::async::CAsyncExecutor exec(2);
    exec.Start();

    for (bool bFail : {false, true})  // 第二遍走风控拒绝分支
    {
        auto spCtx = std::make_shared<CFlowCtx>();
        spCtx->nUserId = 123;
        spCtx->bNoOrders = bFail;

        // 等价 spawn(…).then(…).then(…).recover(…)
        exec.NewPromise(spCtx, &StepFetchUser, ASYNC_LOC)
            .Then(&StepFetchOrders, ASYNC_LOC)
            .Then(&StepCheckRisk, ASYNC_LOC)
            .Then(&StepFetchPayment, ASYNC_LOC)
            .Then(&StepPrintTotal, ASYNC_LOC)
            .Catch(&StepRecover, ASYNC_LOC)
            .Then(&StepDone, ASYNC_LOC)
            .Await();
    }
    return 0;
}
```

编译运行（先 `./build.sh --debug Common` 生成 `build/debug/libCommon.a`）：

```bash
g++ -std=c++11 -Wall -Wextra -O0 -g -pthread -ICommon style_asyncpp.cpp build/debug/libCommon.a -o /tmp/style4 && /tmp/style4
```

```text
用户: Alice
订单数: 2
通过风控，订单: order1
支付: 99
最终总额: 99
流程结束: 资源已回收
用户: Alice
订单数: 0
流程中断 [100]: 没有订单可处理
流程结束: 资源已回收
```

几点：

- `tce.set_exception(...)` / `tce.set_value(...)` → 处理器返回 `Reject(码)` / `Resolve()`，或用 `CPromise<Ctx>::New(exec, spCtx, executor)` 手动 `fnReject(码)` / `fnResolve()`（见 [async-style-manual-settle.md](async-style-manual-settle.md)）。
- `.recover(...)` → `.Catch(处理器)`：返回 `Resolve()` 表示**恢复**，链上后续 `Then` 照常执行；返回原 `upResult` 则继续往外传。
- 两个 `catch` 子句（`RejectReason` / `std::exception`）→ 按码分流：`码 >= kBusinessBase` 是业务拒绝，`kException` / `kRejected` / `kStopped` 是系统侧。
- `chain.get()` / `.wait()` → `.Await()`。
