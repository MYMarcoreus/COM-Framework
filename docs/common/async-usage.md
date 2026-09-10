# 异步链 CAsyncChain — 使用文档

> 对应目录：`Common/Async`（命名空间 `common::async`）
> 实现细节见：[async-impl.md](async-impl.md) ｜ 协程见：[coroutine-usage.md](coroutine-usage.md)

## 1. 这是什么

`CAsyncExecutor` + `CAsyncChain` 是**异步链特化版**框架：把一段业务流程写成若干「层」，
层按注册顺序执行，**层与层之间只传递「本层成功 / 失败」**，数据统一放在**共享上下文**里。

与「层间传任意值」的通用任务链（`Then([](int n) { ... })`）相比，本版的取舍是：

| 维度 | 通用任务链（传值） | 异步链（本框架） |
| --- | --- | --- |
| 层间传什么 | 上一层返回的任意值（类型可变） | 只有成败（`CStepResult`） |
| 数据怎么传 | 返回值逐层往下递 | 共享上下文（`std::shared_ptr<TContext>`） |
| 层函数签名 | 每层不同 | 全部固定 |
| 链的类型 | 随层变化（`CTask<A>` → `CTask<B>`） | 恒为 `CAsyncChain<TContext>` |
| 失败怎么处理 | 无值终止（Option 风格） | 失败即停 + 失败码透传（`ThenAlways` 可回滚） |

固定签名：

```cpp
CStepResult fn(CStepResult upStep,                        // 上一层的结果
               const std::shared_ptr<TContext>& spCtx);   // 共享上下文
```

- `upStep`：上一层回调的结果（第一层恒为成功）。下一层据此判断上一层成败；
- `spCtx`：整条链**共用同一个实例**的数据载体（链持有，恒非空）；
- 返回：本层结果。成功继续下一层，失败终止链（后续 `Then` 层不再执行）。

## 2. 最小示例

```cpp
#include "Async/AsyncChain.h"

namespace no = common::async;

// ① 定义一次流程的共享数据（TContext）：各层读写它。
struct CLoginContext
{
    std::string strAccount;
    std::string strToken;
    std::string strError;
};

// ② 定义层：签名固定，数据从 spCtx 走。
no::CStepResult StepReadParam(no::CStepResult upStep, const std::shared_ptr<CLoginContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;                       // 防御写法（失败即停时本层不会被调用）
    }
    spCtx->strAccount = ReadAccount();
    return spCtx->strAccount.empty() ? no::CStepResult::Failed(kCodeNoAccount)
                                     : no::CStepResult::Ok();
}

no::CStepResult StepVerify(no::CStepResult upStep, const std::shared_ptr<CLoginContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    spCtx->strToken = IssueToken(spCtx->strAccount);
    return no::CStepResult::Ok();
}

// ③ 起链：数据在上下文里，层间只传成败。
no::CAsyncExecutor exec(2);
exec.Start();

std::shared_ptr<CLoginContext> spCtx = std::make_shared<CLoginContext>();
no::CAsyncChain<CLoginContext> chain =
    exec.Submit(spCtx, &StepReadParam, ASYNC_LOC)      // 首层（起点结果视为成功）
        .Then(&StepVerify, ASYNC_LOC)                  // 第二层
        .Then(&StepWriteDb, ASYNC_LOC);                // 第三层
chain.OnCompleted([](no::CStepResult finalStep) { /* 收尾（成功 / 失败都触发） */ });

no::CStepResult r = chain.Get();                       // 阻塞取最终成败
if (r.IsOk())
{
    Use(spCtx->strToken);                              // 数据从上下文取
}
```

要点：

- `exec.Submit(spCtx, 首层)` 起链并**立即投递首层**（异步执行，不在调用线程上跑层函数）；
- `.Then(...)` 追加一层，返回**指向新层的句柄**；
- 只有最后一层的句柄取结果才有意义 —— 写成 `auto tail = exec.Submit(...).Then(...)` 后 `tail.Get()`；
  若丢弃 `Then` 的返回值，`chain.Get()` 等到的只是首层。

## 3. 起链与追加层

| 接口 | 语义 |
| --- | --- |
| `exec.Submit(spCtx, fnStep, loc)` | 起链：创建链并投递首层，返回指向首层的句柄 |
| `CAsyncChain<TContext> chain(exec, spCtx)` | 手工建链（尚未起链），随后 `chain.Submit(fnStep)` |
| `CAsyncChain<TContext> chain(exec)` | 同上，上下文由链**懒创建** |
| `chain.Then(fnStep, loc)` | 追加一层（**失败即停**：上一层失败时本层不执行） |
| `chain.ThenAlways(fnStep, loc)` | 追加一层（**失败也执行**：回滚 / 补偿 / 清理用） |
| `chain.Get()` | 阻塞等待本层结果（不抛异常） |
| `chain.OnCompleted(fnCompleted)` | 注册完成回调（成功 / 失败都触发一次），返回是否注册成功 |
| `chain.GetContext()` | 共享上下文（懒创建，有效链上恒非空） |
| `chain.IsValid()` / `chain.IsCompleted()` | 是否有效 / 本层是否已完成 |
| `chain.Loc()` | 本层注册点源码位置（调试构建有效） |

层函数可以是自由函数、静态成员函数、`std::bind` 结果或 lambda —— 只要签名匹配即可：

```cpp
flow.Submit(lambdaStep, ASYNC_LOC);                                  // lambda
flow.Submit(std::bind(&CService::OnStep, this, std::placeholders::_1,
                      std::placeholders::_2));                       // 成员函数
```

## 4. 共享上下文（唯一数据通道）

上下文是**整条链共用**的一个对象，`shared_ptr` 持有，生命周期与链一致：

```cpp
// 方式 A：外部准备数据后注入（已有请求对象 / 连接上下文等）
std::shared_ptr<CMyContext> spCtx = std::make_shared<CMyContext>();
spCtx->strRequestId = GetRequestId();
no::CAsyncChain<CMyContext> chain(exec, spCtx);

// 方式 B：链内部懒创建（首次 GetContext() 时构造，恒非空）
no::CAsyncChain<CMyContext> chain2(exec);
chain2.GetContext()->nRetry = 3;      // 起链前先填初始数据
chain2.Submit(StepA).Then(StepB);
```

约束与建议：

- `TContext` 只在**真正懒创建**时才要求可默认构造（即 `GetContext()` 被实例化时）；
- 层函数拿到的是 `const std::shared_ptr<TContext>&`（借用引用，不增加引用计数）；
  若要留给**异步回调**使用，自行拷贝该 `shared_ptr` 保活；
- 同一链的层顺序执行，**不会并发**；跨链共享同一上下文时并发安全由业务负责。

## 5. 失败语义

### 5.1 失败即停（`Then`）

某层返回失败后，后续 `Then` 层**不再执行**，失败码沿链透传到最后一层、`Get()` 与
`OnCompleted`。框架保证「忘记写判断也不会误执行后续业务」。

```cpp
// 第 2 层失败 → 第 3、4 层不执行
auto r = exec.Submit(spCtx, &StepReadParam)   // 失败（参数非法）
             .Then(&StepVerify)               // 不执行
             .Then(&StepWriteDb)              // 不执行
             .Get();
// r.IsFailed() == true，r.Code() == 业务错误码
```

### 5.2 失败也执行（`ThenAlways`）

需要「无论成败都要跑」的层（回滚 / 补偿 / 清理 / 审计）用 `ThenAlways`：
它**总会执行**，`upStep` 就是上一层的结果（可能是失败）。

```cpp
no::CStepResult StepRollback(no::CStepResult upStep, const std::shared_ptr<Ctx>& spCtx)
{
    spCtx->strTrace += "回滚;";
    return upStep;                 // 透传失败：后续 Then 层仍不执行
}

no::CStepResult StepRecover(no::CStepResult upStep, const std::shared_ptr<Ctx>& spCtx)
{
    (void)upStep;
    spCtx->strTrace += "恢复;";
    return no::CStepResult::Ok();  // 吞掉失败：链从本层之后继续执行
}
```

### 5.3 错误码约定

```cpp
no::kStepOk            = 0   // 成功
no::kStepFailed        = 1   // 业务失败（未指定码时的默认值）
no::kStepStopped       = 2   // 执行器已停止 / 投递失败（框架）
no::kStepException     = 3   // 层函数抛异常（框架捕获，不向调用方抛出）
no::kStepBusinessBase  = 100 // 业务错误码从 100 起取
```

框架只解释 1..99，其余码**原样透传**（错误码语义由业务定义）。

### 5.4 异常

层函数内抛出的异常被框架捕获并转为**本层失败**（`kStepException`），
`Get()` / `OnCompleted` 不会向调用方抛异常。

## 6. 完成回调与结果

```cpp
// 完成回调：成功与失败都触发一次；可注册多个（分叉时各自触发）
chain.OnCompleted([](no::CStepResult finalStep)
{
    Log(finalStep.IsOk() ? "成功" : ("失败码=" + std::to_string(finalStep.Code())));
});

no::CStepResult r = chain.Get();   // 阻塞等待（多线程可同时等待同一链）
```

注意：`Get()` 返回与完成回调的执行**没有先后保证**（回调在段完成后按注册顺序触发）。
测试里若依赖「回调已跑完」，请另用标志 / 条件变量同步。

## 7. 执行器

```cpp
no::CAsyncExecutor exec(4);             // 4 个工作线程
exec.Start();                           // 启动（未启动时起链立即以 kStepStopped 失败）
exec.Post([]() { /* 无返回值任务 */ });  // fire-and-forget（返回是否提交成功）
exec.IsIdle();                          // 队列是否为空（协程内联续接判断用）
exec.Stop();                            // 停止并等待已投递任务完成
```

- `Post`：不涉及链的一次性任务（重活下沉 / 事件异步分发）；
- 未 `Start()` / 已 `Stop()` 时 `Submit`、`Post` 都不抛异常，而是返回失败 / `false`；
- `Stop()` 之后可再次 `Start()`（重建句柄与线程池，隔离旧任务）。

## 8. 线程模型

| 事实 | 说明 |
| --- | --- |
| 首层 | 由 `Submit` 投递到执行器，**在工作线程上执行** |
| 后续层 | 上游完成时**在同一工作线程上级联执行**（不再逐层入队） |
| 单链并发度 | 一条链的层**顺序执行**，任意时刻只有一个线程在跑它的层 |
| 深链 | 连续内联超过 `kMaxInlineDepth`（64）的层改为投递，防递归爆栈 |
| 分叉 | 同一层可注册多个 `Then`，各自独立延续（可能在不同线程并行） |
| 多链 | 多条链互不阻塞，线程池有界并行 |

层内要并行时，自行投递重活（`exec.Post`）或起子链（见协程文档）。

## 9. 生命周期

- 链句柄是**浅句柄**（拷贝共享同一链的同一段）：句柄存活期间，段与线程池都被保活；
- 链通过共享句柄（`shared_ptr<CExecutorHandle>`）引用执行器线程池：
  **执行器析构后，已起动的链仍安全跑完**，新投递以 `kStepStopped` 失败；
- 无效链（默认构造、未绑定执行器）上 `Submit` / `Then` 为空操作，`Get()` 返回失败。

## 10. 常见用法速查

```cpp
// 单层
no::CStepResult r = exec.Submit(spCtx, StepOne, ASYNC_LOC).Get();

// 多层（失败即停）+ 收尾
auto tail = exec.Submit(spCtx, StepA, ASYNC_LOC).Then(StepB, ASYNC_LOC).Then(StepC, ASYNC_LOC);
tail.OnCompleted([](no::CStepResult r) { /* 成功 / 失败 */ });
no::CStepResult final = tail.Get();

// 回滚（失败也执行）
auto tail2 = exec.Submit(spCtx, StepA, ASYNC_LOC)
                 .Then(StepB, ASYNC_LOC)
                 .ThenAlways(StepRollback, ASYNC_LOC);

// 分叉（同一层两条支线）
no::CAsyncChain<Ctx> head = exec.Submit(spCtx, StepA, ASYNC_LOC);
no::CAsyncChain<Ctx> b1 = head.Then(StepB, ASYNC_LOC);
no::CAsyncChain<Ctx> b2 = head.Then(StepC, ASYNC_LOC);

// 惰性上下文 / 外部注入两种姿势
no::CAsyncChain<Ctx> c1(exec);            // 链内创建
no::CAsyncChain<Ctx> c2(exec, spCtx);     // 外部注入
```

### 与旧版（传值版 `CTask`）的迁移对照

| 旧写法（已移除） | 新写法 |
| --- | --- |
| `exec.Submit([]{ return 3; }).Then([](int n){ return n * 2; })` | 数据放上下文：`spCtx->n = 3;`，层内读改写 |
| `return no::None;`（无值终止） | `return no::CStepResult::Failed(码);` |
| `r.HasValue() / r.Value()` | `r.IsOk() / r.Code()`，数据从 `GetContext()` 取 |
| `OnSuccess / OnNone` | `OnCompleted`（统一一个回调，看 `IsOk()`） |
| `NOTHROW_LOC` | `ASYNC_LOC` |
| flatMap（层返回 `CTask`） | 层内起子链并用协程 await（见 coroutine 文档） |

## 11. 测试与示例

- 示例程序：`examples/main.cpp`（19 个演示，含失败即停、ThenAlways 回滚、深链、协程）；
- 单元测试：`Tests/test_async_chain.cpp`（链 + 协程共 31 个用例）；
- 基准：`Benchmark/cases/ChainCase.cpp`、`CoroutineCase.cpp`、`StressCase.cpp`；
- 运行：`./build.sh --tests`（或 `./build/debug/tests`）、`./build/debug/examples`。
