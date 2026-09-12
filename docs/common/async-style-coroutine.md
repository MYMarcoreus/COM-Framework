# 示例：libgo 风格（一个协程里直线书写）

libgo 的写法：一个协程里逐步 `co_await` 同步书写，业务拒绝抛异常中断，`catch` 统一兜底。

```cpp
void businessFlow()
{
    try
    {
        User user = fetchUser(123);  // 函数体内部 co_await
        auto orders = fetchOrders(user);
        Order order = checkRisk(orders);  // 不满足则 throw RejectReason
        Payment payment = fetchPayment(order);
    }
    catch (const RejectReason& r)
    { /* 业务拒绝 */
    }
    catch (const std::exception& ex)
    { /* 真实异常 */
    }
}

int main()
{
    go businessFlow;
    co_sched.RunUntilNoTask();
}
```

本框架用 `CCoroutine` + `CO_AWAIT` 临摹（可编译运行）：

```cpp
// 对标 libgo：一个协程里同步书写（co_await），业务拒绝中断流程，catch 统一兜底。
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"
#include "Async/Promise.h"

/// 上下文：等价这些写法里沿协程 / 链流动的局部变量（user / orders / order / payment）。
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

/// 等价 libgo 的 `void businessFlow()`：一个协程里顺序书写，每步 CO_AWAIT。
class CBusinessFlow : public common::async::CCoroutine<CFlowCtx>
{
   public:
    using common::async::CCoroutine<CFlowCtx>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(StepFetchUser));     // User user = fetchUser(123);
        CO_AWAIT(NewPromise(StepFetchOrders));   // auto orders = fetchOrders(user);
        CO_AWAIT(NewPromise(StepCheckRisk));     // Order order = checkRisk(orders);（拒绝则终止）
        CO_AWAIT(NewPromise(StepFetchPayment));  // Payment payment = fetchPayment(order);
        CO_AWAIT(NewPromise(StepPrintTotal));    // std::cout << "最终总额: " << payment.total;
        CO_RETURN(common::async::CPromiseResult::Resolve());
        CO_END();
    }

   private:
    /// 步骤 A：fetchUser(123)
    static common::async::CPromiseResult StepFetchUser(common::async::CPromiseResult /*upResult*/,
                                                       const std::shared_ptr<CFlowCtx>& spCtx)
    {
        spCtx->strUserName = "Alice";
        std::printf("用户: %s\n", spCtx->strUserName.c_str());
        return common::async::CPromiseResult::Resolve();
    }

    /// 步骤 B：fetchOrders(user)
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

    /// 步骤 C：checkRisk(orders) —— 条件不满足时 Reject（等价 throw RejectReason）
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

    /// 步骤 D：fetchPayment(order) —— 只有步骤 C 通过才会执行到这里
    static common::async::CPromiseResult StepFetchPayment(common::async::CPromiseResult /*upResult*/,
                                                          const std::shared_ptr<CFlowCtx>& spCtx)
    {
        spCtx->nTotal = 99;
        std::printf("支付: %d\n", spCtx->nTotal);
        return common::async::CPromiseResult::Resolve();
    }

    /// 步骤 E：打印最终总额
    static common::async::CPromiseResult StepPrintTotal(common::async::CPromiseResult /*upResult*/,
                                                        const std::shared_ptr<CFlowCtx>& spCtx)
    {
        std::printf("最终总额: %d\n", spCtx->nTotal);
        return common::async::CPromiseResult::Resolve();
    }
};

/// 统一兜底：等价 `catch (const RejectReason& r)`（只在被拒绝时执行）
static common::async::CPromiseResult OnReject(common::async::CPromiseResult upResult,
                                              const std::shared_ptr<CFlowCtx>& /*spCtx*/)
{
    std::fprintf(stderr, "流程中断 [%d]: %s\n", upResult.Code(), CodeText(upResult.Code()));
    return upResult;  // 原样返回：拒绝继续往后传（等价 catch 里没吞掉）
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

        // 等价 `go businessFlow;` + `co_sched.RunUntilNoTask();`
        common::async::CPromise<CFlowCtx> promise = exec.CoStart<CBusinessFlow>(spCtx)->AsPromise().Catch(OnReject);
        const common::async::CPromiseResult result = promise.Await();
        std::printf("结束: %s(%d)\n", result.IsFulfilled() ? "兑现" : "拒绝", result.Code());
    }
    return 0;
}
```

编译运行（先 `./build.sh --debug Common` 生成 `build/debug/libCommon.a`）：

```bash
g++ -std=c++11 -Wall -Wextra -O0 -g -pthread -ICommon style_coroutine.cpp build/debug/libCommon.a -o /tmp/style1 && /tmp/style1
```

```text
用户: Alice
订单数: 2
通过风控，订单: order1
支付: 99
最终总额: 99
结束: 兑现(0)
用户: Alice
订单数: 0
流程中断 [100]: 没有订单可处理
结束: 拒绝(100)
```

几点：

- `co_await xxx` → `CO_AWAIT(NewPromise(步骤))`：非阻塞挂起（不占 worker），被 await 的步骤被拒绝则协程立即终止，后续 `CO_AWAIT` 不再执行。
- 函数里的局部变量 `user` / `orders` / `order` / `payment` → 上下文 `CFlowCtx` 的字段（协程与它起的子 promise 共用同一实例）。
- `throw RejectReason{code, msg}` → 步骤返回 `CPromiseResult::Reject(码)`：**层间只传码**，文案自己查表（`CodeText`）。
- `try / catch` → 协程 `AsPromise().Catch(处理器)`；处理器返回原 `upResult` 即「catch 没吞掉」，拒绝继续往外传。
- `go businessFlow; co_sched.RunUntilNoTask();` → `exec.CoStart<CBusinessFlow>(spCtx)` 起协程，`Await()` 等它跑完。
