# 精简示例：一条 promise 链里混用多种 then

> 完整版（4 条路径 + 执行器隔离自校验）：[`examples/cases/ThenMixCase.cpp`](../../examples/cases/ThenMixCase.cpp)
> 语义与 API 速查：[async-usage.md](async-usage.md)

## 这个例子教什么

一条**下单流程**串起 then 的全部常见写法，全程零阻塞、不用协程：

| 层 | 写法（本示例里的名字） | 一句话语义 |
|---|---|---|
| ① | 具名异步函数 `StepLoad` | 复用 / 可单测的逻辑；模拟一次 IO |
| ② | lambda `fnValidate` | 只此一处用的小逻辑，就地写 |
| ③ | 工厂 `fnQueryStock` + 桥接 `BridgeQuery`（模块内部 `StepConnect` / `StepRead`） | **调用别的模块**的异步函数，并**等它**（跨上下文） |
| ④ | 工厂 `fnReserve` + 内层链步骤 `StepReserve` | **内部现搭一条链**，并**等它**（同上下文，直接 adopt） |
| ⑤ | lambda `fnBilling` | **旁支 / fire-and-forget**：主链不等它 |
| catch | `fnCompensate` | 只在上游**被拒绝**时执行（返回 `upResult` = 透传拒绝） |
| finally | `fnAudit` | 成败都执行、返回值被忽略、原样透传结果 |

## 代码（单文件，可直接编译）

```cpp
// 精简版：一条 promise 链里混用多种 then（下单流程）
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

// ---------------- 库存模块：自持执行器，对外只给 promise ----------------

struct CStockCtx
{
    int nAvail = 0;  ///< 可售量。
};

/// ③-1 库存模块内部：建连（调用方不关心它是几步、跑在哪个线程）
static no::CPromiseResult StepConnect(no::CPromiseResult /*up*/, const std::shared_ptr<CStockCtx>& /*sp*/)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(2));  // 模拟握手
    return no::CPromiseResult::Resolve();
}

/// ③-2 库存模块内部：读可售量
static no::CPromiseResult StepRead(no::CPromiseResult /*up*/, const std::shared_ptr<CStockCtx>& sp)
{
    sp->nAvail = 5;  // 演示数据：可售 5 件
    return no::CPromiseResult::Resolve();
}

class CStockModule
{
   public:
    CStockModule() : m_exec(1)
    {
        m_exec.Start();
    }

    /// @brief 异步查库存：在**自己的**执行器上跑两步，调用方只拿 promise。
    no::CPromise<CStockCtx> QueryAsync()
    {
        auto spStock = std::make_shared<CStockCtx>();
        return m_exec.NewPromise(spStock, &StepConnect, ASYNC_LOC).Then(&StepRead, ASYNC_LOC);
    }

   private:
    no::CAsyncExecutor m_exec;  ///< 模块私有执行器（析构自动 Stop）。
};

// ---------------- 下单流程：自己的执行器 + 自己的上下文 ----------------

struct COrderCtx
{
    int nQty = 5;        ///< 购买数量。
    int nStock = 0;      ///< 库存模块返回的可售量。
    int nTotal = 0;      ///< 应付合计。
    std::string strLog;  ///< 执行轨迹。
};

using COrderP = no::CPromise<COrderCtx>;

/// ① 具名异步函数：读订单（模拟 IO）
static no::CPromiseResult StepLoad(no::CPromiseResult /*up*/, const std::shared_ptr<COrderCtx>& sp)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    sp->nTotal = 12 * sp->nQty;
    sp->strLog += "读订单;";
    return no::CPromiseResult::Resolve();
}

/// ④ 内层链的一步（同上下文）
static no::CPromiseResult StepReserve(no::CPromiseResult /*up*/, const std::shared_ptr<COrderCtx>& sp)
{
    sp->strLog += "预占;";
    return no::CPromiseResult::Resolve();
}

/// ③ 桥接：把「库存模块的 promise」接进本流程（等价 JS 的 new Promise）
static COrderP BridgeQuery(no::CAsyncExecutor& exec, const std::shared_ptr<CStockModule>& spStock,
                           const std::shared_ptr<COrderCtx>& sp)
{
    COrderP::PromiseExecutor fnExecutor =
        [spStock, sp](const COrderP::ResolveFn& fnResolve, const COrderP::RejectFn& fnReject)
    {
        auto pStock = spStock->QueryAsync();    // 发起跨模块调用（不等待）
        auto spStockCtx = pStock.GetContext();  // 对方的上下文
        pStock.OnSettled([sp, spStockCtx, fnResolve, fnReject](no::CPromiseResult result)
        {
            // 本回调跑在**库存模块的线程**上：只做语义转换 + 改上下文 + settle 本层
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
    return COrderP::New(exec, sp, fnExecutor, ASYNC_LOC);
}

/// 组装下单流程：把 ①~⑤ + catch + finally 串起来（一行一个 then，与编号一一对应）
///
/// @param exec 下单流程自己的执行器。
/// @param sp 下单流程上下文。
/// @param spStockModule 库存模块（自持执行器，调用方只拿它的 promise）。
///
/// @return 指向 finally 层的 promise。
static COrderP BuildOrderFlow(no::CAsyncExecutor& exec, const std::shared_ptr<COrderCtx>& sp,
                              const std::shared_ptr<CStockModule>& spStockModule)
{
    // ② lambda：只此一处用的小逻辑（校验）—— 先赋给具名变量再串链，排版稳定
    COrderP::ThenHandler fnValidate = [](no::CPromiseResult, const std::shared_ptr<COrderCtx>& spSelf)
    {
        if (spSelf->nQty <= 0 || spSelf->nQty > 10)
        {
            return no::CPromiseResult::Reject(kCodeBadOrder);  // 本层拒绝 → 后续 then 不执行
        }
        spSelf->strLog += "校验;";
        return no::CPromiseResult::Resolve();
    };

    // ③ 工厂：调库存模块的异步函数（跨上下文，经 BridgeQuery 桥接）
    COrderP::PromiseFactory fnQueryStock = [&exec, spStockModule](const std::shared_ptr<COrderCtx>& spSelf)
    {
        return BridgeQuery(exec, spStockModule, spSelf);
    };

    // ④ 工厂：现搭一条内层链，让它参与当前链（同上下文，直接 adopt）
    COrderP::PromiseFactory fnReserve = [&exec](const std::shared_ptr<COrderCtx>& spSelf)
    {
        return exec.NewPromise(spSelf, &StepReserve, ASYNC_LOC);
    };

    // ⑤ lambda：旁支 —— 只要**不返回**新链，主链就不等它（fire-and-forget）
    COrderP::ThenHandler fnBilling = [](no::CPromiseResult, const std::shared_ptr<COrderCtx>& spSelf)
    {
        spSelf->strLog += "记账已发起(不等);";
        return no::CPromiseResult::Resolve();
    };

    // catch 处理器：只在上游被拒绝时执行（返回 up = 透传拒绝；返回 Resolve() 则吞掉拒绝继续）
    COrderP::ThenHandler fnCompensate = [](no::CPromiseResult up, const std::shared_ptr<COrderCtx>& spSelf)
    {
        spSelf->strLog += "补偿;";
        return up;
    };

    // finally 处理器：成败都执行、返回值被忽略（原样透传上一层结果）
    COrderP::ThenHandler fnAudit = [](no::CPromiseResult up, const std::shared_ptr<COrderCtx>& spSelf)
    {
        spSelf->strLog += "审计;";
        return up;
    };

    return exec
        .NewPromise(sp, &StepLoad, ASYNC_LOC)  // ① 具名异步函数
        .Then(fnValidate, ASYNC_LOC)           // ② lambda：校验
        .ThenPromise(fnQueryStock, ASYNC_LOC)  // ③ 调库存模块（**等它**）
        .ThenPromise(fnReserve, ASYNC_LOC)     // ④ 内层链（**等它**）
        .Then(fnBilling, ASYNC_LOC)            // ⑤ 旁支（**不等它**）
        .Catch(fnCompensate, ASYNC_LOC)        // catch：仅被拒绝时执行
        .Finally(fnAudit, ASYNC_LOC);          // finally：成败都跑、不改结果
}

int main()
{
    no::CAsyncExecutor exec(2);                             // 下单流程自己的执行器
    auto spStockModule = std::make_shared<CStockModule>();  // 库存模块（自持执行器）
    auto sp = std::make_shared<COrderCtx>();
    exec.Start();

    // 仅 main 取结果用；业务代码请用 OnSettled 回调（不阻塞）
    const no::CPromiseResult result = BuildOrderFlow(exec, sp, spStockModule).Await();

    std::printf("结果=%s 合计=%d 库存=%d 轨迹=%s\n", result.IsFulfilled() ? "兑现" : "拒绝", sp->nTotal, sp->nStock,
                sp->strLog.c_str());

    exec.Stop();  // 库存模块的执行器随模块析构自动停
    return 0;
}
```

编译（先 `./build.sh --debug Common` 生成 `build/debug/libCommon.a`）：

```bash
g++ -std=c++11 -Wall -Wextra -O0 -g -pthread -ICommon min_then.cpp build/debug/libCommon.a -o /tmp/min_then && /tmp/min_then
```

> **排版**：上面代码按项目 `.clang-format` 写（4 空格缩进、Allman 大括号、≤ 120 列）。
> 若把代码另存到工作区**之外**（如 `/tmp/x.cpp`），编辑器找不到项目 `.clang-format`，
> clangd 会退化成默认 LLVM/Google 风格（2 空格、大括号同行、`& ` 空格）——
> 要么把文件放进工作区，要么把 `.clang-format` 一并拷到同目录。

## 运行结果

正常路径（`nQty = 5`）：

```text
结果=兑现 合计=60 库存=5 轨迹=读订单;校验;查库存(5);预占;记账已发起(不等);审计;
```

② 拒绝路径（把 `COrderCtx::nQty` 改成 `50`）：

```text
结果=拒绝 合计=600 库存=0 轨迹=读订单;补偿;审计;
```

第二条说明「失败即停」：② 拒绝后 ③④⑤ 全部不执行（没有 `查库存`、`预占`、`记账已发起`），
`Catch` 与 `Finally` 仍然执行 —— 且轨迹里的 `补偿;` 在 `审计;` 之前。

## 四个要点

1. **顺序由「依赖边」保证，不靠共享线程。**
   ③ 的链在库存模块的线程上跑，④/⑤ 由 `OnSettled → fnResolve() → 本层 settle → 下一层` 串起来，
   步骤之间有 happens-before，所以 `查库存(5);` 必然排在 `预占;` 前面。
   唯一**不保证**先后的是旁支（⑤，故意不等它）。
2. **执行器是模块的私有资源，不跨模块传递。**
   库存模块自持 `exec(1)`，只对外给「自己上下文的 promise」；调用方拿不到也无需知道它有几个 worker。
   （续跑线程通常就是「结算它的那条线程」，所以跨模块回调里只做轻活；要回到本模块线程就显式 `exec.Post(...)`。）
3. **then 里不用判断上一层结果。** 上游被拒绝时框架直接跳过本层；要看拒绝用 `Catch`，
   要成败都收尾用 `Finally`（返回值被忽略）。
4. **要「等」就返回 promise（`ThenPromise`）；不返回就只是旁支。**
   普通 `Then` 的处理器只能返回 `CPromiseResult`，所以里面起的链主链一概不等。

## 与 JS 的对应

| JS | 本示例 |
|---|---|
| `p.then(v => f(v))` | `.Then(handler)` |
| `p.then(v => inner())`（返回 promise → 自动等待） | `.ThenPromise(factory)` |
| `new Promise((resolve, reject) => {...})` | `CPromise::New(exec, spCtx, executor, ASYNC_LOC)` |
| `p.catch(e => {...})` / `p.finally(() => {...})` | `.Catch(handler)` / `.Finally(handler)` |
| `p.finally(() => audit())`（旁路观察） | `.OnSettled(cb)`（不在链上、不改结果） |
| 在 `then` 里调用内部异步但不 `return` | `.Then` 里起链、不返回（fire-and-forget 旁支） |
