# 示例：JavaScript Promise 风格（then 链 + reject + catch）

JS 的写法：外层 `then` 里 `return` 内层链；`checkRisk` 条件不满足时 `return Promise.reject(reason)`，
链上后续 `then` 全部跳过，最后 `.catch(reason => …)` 统一兜底（reason 可以是任意类型）。

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

本框架写法（可编译运行）：

```cpp
// 对标 JS Promise：外层 then 里 return 内层链；checkRisk 拒绝即中断，catch 统一兜底。
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"

namespace no = common::async;

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

/// 业务拒绝码（从 kBusinessBase 起取；等价 reject reason 里的 code）。
enum
{
    kNoOrders = no::kBusinessBase,
    kTooMany
};

/// 拒绝码 → 文案（层间只传码，文案自己查表；等价 reject reason 里的 msg）。
static const char* CodeText(int nCode)
{
    switch (nCode)
    {
        case kNoOrders:
            return "没有订单可处理";
        case kTooMany:
            return "订单过多，需人工审核";
        case no::kException:
            return "系统错误";
        default:
            return "未知错误";
    }
}

/// 步骤 A：fetchUser(123)
static no::CPromiseResult StepFetchUser(no::CPromiseResult /*upResult*/, const std::shared_ptr<CFlowCtx>& spCtx)
{
    spCtx->strUserName = "Alice";
    std::printf("用户: %s\n", spCtx->strUserName.c_str());
    return no::CPromiseResult::Resolve();
}

/// 步骤 B：fetchOrders(user)
static no::CPromiseResult StepFetchOrders(no::CPromiseResult /*upResult*/, const std::shared_ptr<CFlowCtx>& spCtx)
{
    if (!spCtx->bNoOrders)
    {
        spCtx->vecOrders.push_back("order1");
        spCtx->vecOrders.push_back("order2");
    }
    std::printf("订单数: %zu\n", spCtx->vecOrders.size());
    return no::CPromiseResult::Resolve();
}

/// 步骤 C：checkRisk(orders) —— 条件不满足时返回 Reject（等价 `return Promise.reject(reason)`）
static no::CPromiseResult StepCheckRisk(no::CPromiseResult /*upResult*/, const std::shared_ptr<CFlowCtx>& spCtx)
{
    if (spCtx->vecOrders.empty())
    {
        return no::CPromiseResult::Reject(kNoOrders);
    }
    if (spCtx->vecOrders.size() > 10)
    {
        return no::CPromiseResult::Reject(kTooMany);
    }
    spCtx->strOrderId = spCtx->vecOrders.front();
    std::printf("通过风控，订单: %s\n", spCtx->strOrderId.c_str());
    return no::CPromiseResult::Resolve();
}

/// 步骤 D：fetchPayment(order) —— 只有 checkRisk 兑现才会执行
static no::CPromiseResult StepFetchPayment(no::CPromiseResult /*upResult*/, const std::shared_ptr<CFlowCtx>& spCtx)
{
    spCtx->nTotal = 99;
    std::printf("支付: %d\n", spCtx->nTotal);
    return no::CPromiseResult::Resolve();
}

/// 步骤 E：`.then(total => …)`
static no::CPromiseResult StepPrintTotal(no::CPromiseResult /*upResult*/, const std::shared_ptr<CFlowCtx>& spCtx)
{
    std::printf("最终总额: %d\n", spCtx->nTotal);
    return no::CPromiseResult::Resolve();
}

/// `.catch(reason => …)`：只在被拒绝时执行
static no::CPromiseResult OnReject(no::CPromiseResult upResult, const std::shared_ptr<CFlowCtx>& /*spCtx*/)
{
    std::fprintf(stderr, "流程中断 [%d]: %s\n", upResult.Code(), CodeText(upResult.Code()));
    return upResult;  // 原样返回：拒绝继续往后传（等价 catch 里没吞掉）
}

int main()
{
    no::CAsyncExecutor exec(2);
    exec.Start();

    for (bool bFail : {false, true})  // 第二遍走风控拒绝分支
    {
        auto spCtx = std::make_shared<CFlowCtx>();
        spCtx->nUserId = 123;
        spCtx->bNoOrders = bFail;

        // `.then(user => { return 内层链 })`：内层链 = fetchOrders → checkRisk → fetchPayment → total
        no::CPromise<CFlowCtx>::PromiseFactory fnInner = [&exec](const std::shared_ptr<CFlowCtx>& spSelf)
        {
            return exec.NewPromise(spSelf, &StepFetchOrders, ASYNC_LOC)
                .Then(&StepCheckRisk, ASYNC_LOC)
                .Then(&StepFetchPayment, ASYNC_LOC)
                .Then(&StepPrintTotal, ASYNC_LOC);
        };

        // 等价 fetchUser(123).then(…).then(total => …).catch(reason => …)
        exec.NewPromise(spCtx, &StepFetchUser, ASYNC_LOC)
            .ThenPromise(fnInner, ASYNC_LOC)
            .Catch(&OnReject, ASYNC_LOC)
            .Await();
    }
    return 0;
}
```

编译运行（先 `./build.sh --debug Common` 生成 `build/debug/libCommon.a`）：

```bash
g++ -std=c++11 -Wall -Wextra -O0 -g -pthread -ICommon style_js_then.cpp build/debug/libCommon.a -o /tmp/style2 && /tmp/style2
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

- `.then(回调)` → `Then(处理器)`；每层的数据是上下文 `CFlowCtx`，不是沿链流动的值。
- `.then(user => { return 内层链 })` → `ThenPromise(工厂)`：外层等内层链 settle 后才继续（回调返回 promise 会被展平这件事，本框架要显式写成 `ThenPromise`）。
- `return Promise.reject(reason)` → 处理器返回 `CPromiseResult::Reject(码)`：拒绝即停，后面 `Then` 全跳过。
- `.catch(reason => …)` → `Catch(处理器)`；返回原 `upResult` 表示「没吞掉」，拒绝继续往后传；返回 `Resolve()` 则恢复（见 [async-style-recover.md](async-style-recover.md)）。
- reason 可以是任意类型 → 本框架只有 `int` 码（业务码从 `kBusinessBase` 起取），文案自己查表。
