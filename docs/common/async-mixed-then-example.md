# 示例：一条 promise 链里混用多种 then

单文件，直接编译可跑。链上一行一个 then，注释里的编号与实现它的函数一一对应。

## 代码

```cpp
// 一条 promise 链里混用多种 then（下单流程）
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"

namespace no = common::async;

/// 业务错误码（从 kBusinessBase 起取）
enum MinCode
{
    kCodeBadOrder = no::kBusinessBase + 1  ///< 订单参数非法。
};

// ====================================================================
// 库存模块：自持执行器，对外只给 promise
// ====================================================================

struct CStockCtx
{
    int nAvail = 0;  ///< 可售量。
};

class CStockModule
{
   public:
    CStockModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 异步查库存：在模块自己的执行器上跑两步。
    ///
    /// @return 库存上下文的 promise（调用方只能等它）。
    no::CPromise<CStockCtx> QueryAsync()
    {
        auto spStock = std::make_shared<CStockCtx>();
        return m_exec.NewPromise(spStock, &StepConnect, ASYNC_LOC).Then(&StepRead, ASYNC_LOC);
    }

   private:
    /// ③-1 建连（模拟握手）
    static no::CPromiseResult StepConnect(no::CPromiseResult /*up*/, const std::shared_ptr<CStockCtx>& /*sp*/)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return no::CPromiseResult::Resolve();
    }

    /// ③-2 读可售量
    static no::CPromiseResult StepRead(no::CPromiseResult /*up*/, const std::shared_ptr<CStockCtx>& sp)
    {
        sp->nAvail = 5;  // 演示数据：可售 5 件
        return no::CPromiseResult::Resolve();
    }

    no::CAsyncExecutor m_exec;  ///< 模块私有执行器（析构自动 Stop）。
};

// ====================================================================
// 下单模块：同样自持执行器，链就在这里组装
// ====================================================================

struct COrderCtx
{
    int nQty = 5;        ///< 购买数量。
    int nStock = 0;      ///< 库存模块返回的可售量。
    int nTotal = 0;      ///< 应付合计。
    std::string strLog;  ///< 执行轨迹。
};

using COrderP = no::CPromise<COrderCtx>;

class COrderModule
{
   public:
    COrderModule() : m_exec(2)
    {
        m_exec.Start();
    }

    /// @brief 异步下单：①~⑤ + catch + finally 串成一条链。
    ///
    /// 链里的 lambda 捕获 this，模块要比链活得久（本示例 Await 完才析构）。
    ///
    /// @param sp 下单上下文。
    /// @param spStockModule 库存模块（调用方只拿它的 promise，拿不到它的执行器）。
    ///
    /// @return 指向 finally 层的 promise。
    COrderP PlaceOrderAsync(const std::shared_ptr<COrderCtx>& sp, const std::shared_ptr<CStockModule>& spStockModule)
    {
        // ③ 工厂：调库存模块，跨上下文经 BridgeQuery 桥接
        COrderP::PromiseFactory fnQueryStock = [this, spStockModule](const std::shared_ptr<COrderCtx>& spSelf)
        {
            return BridgeQuery(spSelf, spStockModule);
        };

        // ④ 工厂：现搭一条内层链，让它参与当前链（同上下文，直接 adopt）
        COrderP::PromiseFactory fnReserve = [this](const std::shared_ptr<COrderCtx>& spSelf)
        {
            return m_exec.NewPromise(spSelf, &StepReserve, ASYNC_LOC);
        };

        return m_exec
            .NewPromise(sp, &StepLoad, ASYNC_LOC)  // ① 读订单
            .Then(&StepValidate, ASYNC_LOC)        // ② 校验
            .ThenPromise(fnQueryStock, ASYNC_LOC)  // ③ 查库存（等它）
            .ThenPromise(fnReserve, ASYNC_LOC)     // ④ 预占（等它）
            .Then(&StepBilling, ASYNC_LOC)         // ⑤ 记账旁支（不等它）
            .Catch(&StepCompensate, ASYNC_LOC)     // catch：仅被拒绝时执行
            .Finally(&StepAudit, ASYNC_LOC);       // finally：成败都跑
    }

   private:
    /// ① 读订单（模拟 IO）
    static no::CPromiseResult StepLoad(no::CPromiseResult /*up*/, const std::shared_ptr<COrderCtx>& sp)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        sp->nTotal = 12 * sp->nQty;
        sp->strLog += "读订单;";
        return no::CPromiseResult::Resolve();
    }

    /// ② 校验
    static no::CPromiseResult StepValidate(no::CPromiseResult /*up*/, const std::shared_ptr<COrderCtx>& sp)
    {
        if (sp->nQty <= 0 || sp->nQty > 10)
        {
            return no::CPromiseResult::Reject(kCodeBadOrder);  // 本层拒绝 → 后续 then 不执行
        }
        sp->strLog += "校验;";
        return no::CPromiseResult::Resolve();
    }

    /// ③ 桥接：把库存模块的 promise 接进本流程（等价 JS 的 new Promise）
    ///
    /// @param sp 下单上下文。
    /// @param spStockModule 库存模块。
    ///
    /// @return 由库存模块的回调 settle 的本流程 promise。
    COrderP BridgeQuery(const std::shared_ptr<COrderCtx>& sp, const std::shared_ptr<CStockModule>& spStockModule)
    {
        COrderP::PromiseExecutor fnExecutor =
            [spStockModule, sp](const COrderP::ResolveFn& fnResolve, const COrderP::RejectFn& fnReject)
        {
            auto pStock = spStockModule->QueryAsync();  // 发起跨模块调用（不等待）
            auto spStockCtx = pStock.GetContext();
            pStock.OnSettled([sp, spStockCtx, fnResolve, fnReject](no::CPromiseResult result)
            {
                // 本回调在库存模块的线程上：只做语义转换 + 改上下文 + settle
                if (result.IsRejected())
                {
                    fnReject(result.Code());  // 跨模块拒绝码 → 本流程拒绝
                    return;
                }
                sp->nStock = spStockCtx->nAvail;
                sp->strLog += "查库存(" + std::to_string(spStockCtx->nAvail) + ");";
                fnResolve();
            });
        };
        return COrderP::New(m_exec, sp, fnExecutor, ASYNC_LOC);
    }

    /// ④ 预占（内层链的一步）
    static no::CPromiseResult StepReserve(no::CPromiseResult /*up*/, const std::shared_ptr<COrderCtx>& sp)
    {
        sp->strLog += "预占;";
        return no::CPromiseResult::Resolve();
    }

    /// ⑤ 记账旁支：这里起链但不返回，主链就不等它
    static no::CPromiseResult StepBilling(no::CPromiseResult /*up*/, const std::shared_ptr<COrderCtx>& sp)
    {
        sp->strLog += "记账已发起(不等);";
        return no::CPromiseResult::Resolve();
    }

    /// catch 补偿：只在上游被拒绝时执行（返回 up 即透传拒绝）
    static no::CPromiseResult StepCompensate(no::CPromiseResult up, const std::shared_ptr<COrderCtx>& sp)
    {
        sp->strLog += "补偿;";
        return up;
    }

    /// finally 审计：成败都执行，返回值被忽略
    static no::CPromiseResult StepAudit(no::CPromiseResult up, const std::shared_ptr<COrderCtx>& sp)
    {
        sp->strLog += "审计;";
        return up;
    }

    no::CAsyncExecutor m_exec;  ///< 模块私有执行器（析构自动 Stop）。
};

int main()
{
    auto spOrderModule = std::make_shared<COrderModule>();  // 下单模块（自持执行器）
    auto spStockModule = std::make_shared<CStockModule>();  // 库存模块（自持执行器）
    auto sp = std::make_shared<COrderCtx>();

    // main 只取结果；业务代码用 OnSettled 回调
    const no::CPromiseResult result = spOrderModule->PlaceOrderAsync(sp, spStockModule).Await();

    std::printf("结果=%s 合计=%d 库存=%d 轨迹=%s\n", result.IsFulfilled() ? "兑现" : "拒绝", sp->nTotal, sp->nStock,
                sp->strLog.c_str());
    return 0;
}
```

编译（先 `./build.sh --debug Common` 生成 `build/debug/libCommon.a`）：

```bash
g++ -std=c++11 -Wall -Wextra -O0 -g -pthread -ICommon min_then.cpp build/debug/libCommon.a -o /tmp/min_then && /tmp/min_then
```

输出：

```text
结果=兑现 合计=60 库存=5 轨迹=读订单;校验;查库存(5);预占;记账已发起(不等);审计;
结果=拒绝 合计=600 库存=0 轨迹=读订单;补偿;审计;
```

第二行是把 `COrderCtx::nQty` 改成 `50` 后的结果：② 拒绝，③④⑤ 都不执行，catch / finally 照跑。

几点说明（代码注释里也标了）：

- ③ 的链跑在库存模块自己的线程池上，先后顺序靠 `OnSettled → fnResolve → 本层 settle → 下一层`
  这条依赖边保证，不靠共享线程；唯一不保证先后的是旁支 ⑤。
- 执行器是模块私有资源，不跨模块传；调用方只拿对方的 promise（跨上下文用 `CPromise::New` 桥接）。
- then 里不用判断上一层：上游被拒绝时框架直接跳过本层。要看拒绝用 `Catch`，成败都收尾用 `Finally`。
- 要「等」子链就返回它（`ThenPromise`）；普通 `Then` 的处理器只能返回 `CPromiseResult`，里面起的链主链一概不等。

完整版（正常 / 库存不足 / 参数非法 / 内层链拒绝 四条路径 + 自校验）见
[`examples/cases/ThenMixCase.cpp`](../../examples/cases/ThenMixCase.cpp)；API 与语义速查见 [async-usage.md](async-usage.md)。
