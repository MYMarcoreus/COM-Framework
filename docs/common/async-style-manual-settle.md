# 示例：async_promise 风格（显式 resolve / reject）

async_promise 的写法：`make_promise([](resolve, reject) { … })` 里显式调用 `reject(自定义 reason)`
或 `resolve(值)`；`then` 回调里返回 promise 会被展平，`.fail(...)` 统一兜底。

```cpp
async::promise<Order> checkRiskAsync(const std::vector<Order>& orders)
{
    return async::make_promise([orders](auto resolve, auto reject)
    {
        if (orders.empty())
        {
            reject(RejectReason{"NO_ORDERS", "没有订单可处理"});  // ✅ 显式 reject
            return;
        }
        resolve(orders[0]);  // ✅ 显式 resolve
    });
}
```

本框架写法（可编译运行）：

```cpp
// 对标 async_promise：checkRisk 用 CPromise::New 显式兑现 / 拒绝（等价 make_promise(resolve, reject)）。
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

/// checkRiskAsync：等价 `make_promise([](resolve, reject) { … })` —— 显式兑现 / 拒绝本层
static common::async::CPromise<CFlowCtx> CheckRiskAsync(common::async::CAsyncExecutor& exec,
                                                        const std::shared_ptr<CFlowCtx>& spCtx)
{
    common::async::CPromise<CFlowCtx>::PromiseExecutor fnExecutive =
        [spCtx](const common::async::CPromise<CFlowCtx>::ResolveFn& fnResolve,
                const common::async::CPromise<CFlowCtx>::RejectFn& fnReject)
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

    return common::async::CPromise<CFlowCtx>::New(exec, spCtx, fnExecutive, ASYNC_LOC);
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

/// `.fail(...)`：只在被拒绝时执行
static common::async::CPromiseResult OnReject(common::async::CPromiseResult upResult,
                                              const std::shared_ptr<CFlowCtx>& /*spCtx*/)
{
    std::fprintf(stderr, "流程中断 [%d]: %s\n", upResult.Code(), CodeText(upResult.Code()));
    return upResult;  // 原样返回：拒绝继续往后传
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

        // checkRisk 返回一条子 promise（等价 then 回调里返回 promise）
        common::async::CPromise<CFlowCtx>::PromiseFactory fnCheckRisk = [&exec](const std::shared_ptr<CFlowCtx>& spSelf)
        {
            return CheckRiskAsync(exec, spSelf);
        };

        // 等价 fetchUserAsync(id).then(…).then(…).fail(...)
        exec.NewPromise(spCtx, &StepFetchUser, ASYNC_LOC)
            .Then(&StepFetchOrders, ASYNC_LOC)
            .ThenPromise(fnCheckRisk, ASYNC_LOC)
            .Then(&StepFetchPayment, ASYNC_LOC)
            .Then(&StepPrintTotal, ASYNC_LOC)
            .Catch(&OnReject, ASYNC_LOC)
            .Await();
    }
    return 0;
}
```

编译运行（先 `./build.sh --debug Common` 生成 `build/debug/libCommon.a`）：

```bash
g++ -std=c++11 -Wall -Wextra -O0 -g -pthread -ICommon style_async_promise.cpp build/debug/libCommon.a -o /tmp/style3 && /tmp/style3
```

```text
用户: Alice
订单数: 2
通过风控，订单: order1
支付: 99
最终总额: 99
用户: Alice
订单数: 0
流程中断 [100]: 没有订单可处理
```

几点：

- `make_promise([](resolve, reject) { … })` → `CPromise<Ctx>::New(exec, spCtx, executor, ASYNC_LOC)`；executor **同步执行**，里面只发起动作 + 登记回调，不占 worker。
- `reject(RejectReason{code, msg})` → `fnReject(码)`（reason 只剩 `int` 码）；`resolve(orders[0])` → `fnResolve()`（值写进上下文）。
- `then` 回调里 `return` promise → `.ThenPromise(工厂)` 接住该内层 promise：外层等它 settle 才继续，拒绝码原样透传。
- `.fail(...)` → `.Catch(处理器)`（只在被拒绝时执行）；处理器返回原 `upResult` 即继续往后传。
