# 异步框架 vs JavaScript Promise

本框架的命名与语义按 JS 的 Promise / async-await 设计，能直接套用已有直觉；但 C++ 没有动态类型、
没有单线程事件循环，所以有几处必须换写法。冒烟测试见 [`Tests/test_async_smoke.cpp`](../../Tests/test_async_smoke.cpp)，
用法与 API 见 [async-usage.md](async-usage.md)。

## 1. 一一对应

| JavaScript | 本框架 |
|---|---|
| `new Promise(executor)` | `exec.NewPromise(spCtx, 首层)`；由外部 settle 的用 `CPromise<Ctx>::New(exec, spCtx, executor)` |
| `p.then(onFulfilled)` | `p.Then(handler)` |
| `onFulfilled` 返回 promise（自动等待） | `p.ThenPromise(factory)`（factory 返回一条子 promise） |
| `p.catch(onRejected)` | `p.Catch(handler)` |
| `p.finally(onFinally)` | `p.Finally(handler)`（链上的一层）；只做旁路观察用 `p.OnSettled(cb)` |
| `resolve()` / `reject(reason)` | `CPromiseResult::Resolve()` / `CPromiseResult::Reject(码)` |
| `fulfilled` / `rejected` | `result.IsFulfilled()` / `result.IsRejected()` |
| `await p` | `p.Await()`（阻塞，占住一个 worker）；脚本外更推荐协程 `CO_AWAIT(p)`（非阻塞挂起） |
| `p` 已完成 | `p.IsSettled()` |
| `Promise.all([a, b, c])` | 协程 `CO_AWAIT_ALL(a, b, c)`；纯异步下用计数 + `OnSettled` 自己汇总 |
| `Promise.allSettled` / `race` / `any` | 无内建；按同样思路用 `OnSettled` + 计数器/标志位实现 |
| `Promise.resolve(x)` / `Promise.reject(e)` | `CPromiseResult::Resolve()` / `Reject(码)`（结构化的层结果，不是通用工具函数） |
| `async function` | 协程函数（`Common/Async/Coroutine.h`），或纯异步的「层函数 + 链」 |
| `setTimeout(fn, ms)` | `exec.Post(fn)`（下一轮投递）/ 定时器组件 |

## 2. 语义差异

### 2.1 层间不传值，数据走共享上下文

JS 的 `then` 把上一层的返回值交给下一层；本框架的层与层之间**只传成败**，所有数据放在一个共享上下文里，
整条链共用同一实例：

```js
// JS
const order = await loadOrder();
return applyDiscount(order);   // 值沿链流动
```

```cpp
// 本框架：数据写进 spCtx，层间只传 CPromiseResult
static no::CPromiseResult StepLoad(no::CPromiseResult /*up*/, const std::shared_ptr<COrderCtx>& spCtx)
{
    spCtx->nTotal = 12 * spCtx->nQty;
    return no::CPromiseResult::Resolve();
}
```

好处是处理器签名固定、可复用可单测；代价是「把数据当返回值传给下一层」这种写法要改成写上下文。

### 2.2 then 的失败即停、catch / finally：与 JS 一致

- 上游被拒绝时，后续 `Then` 层**不会被执行**，拒绝码直接透传（= JS 的 rejection 跳过 onFulfilled）；
- `Catch` 只在被拒绝时执行，返回 `Resolve()` 即吞掉拒绝继续（= JS 的 `catch` 返回普通值）；
  返回 `upResult` 即继续透传（= JS 的 `throw e`）；
- `Finally` 成败都执行、**忽略返回值**、原样透传上层结果（= JS 的 `finally`）。

所以 then 处理器里**不需要**判断上一层结果（框架保证被调用即兑现）：

```cpp
// 冗余：上游被拒绝时本层根本不会被调用
if (upResult.IsRejected()) { return upResult; }
```

### 2.3 「等子 promise」要显式写 ThenPromise

JS 在 `then` 里 `return` 一个 thenable，引擎自动 flatten；C++ 编译期无法靠返回值判断，所以
「这一层要等一条子 promise」必须用 `ThenPromise`：

```js
p.then(v => fetchStock(v)).then(v => save(v));          // JS：自动等 fetchStock
p.then(v => { fetchStock(v); });                         // JS：不等（旁支）
```

```cpp
p.ThenPromise([&exec](const std::shared_ptr<Ctx>& sp) { return BridgeQuery(exec, sp); }, ASYNC_LOC)  // 等
 .Then(&StepSave, ASYNC_LOC);

p.Then([&exec](no::CPromiseResult, const std::shared_ptr<Ctx>& sp)                                  // 不等（旁支）
       { exec.NewPromise(sp, &StepLog, ASYNC_LOC); return no::CPromiseResult::Resolve(); }, ASYNC_LOC);
```

普通 `Then` 的处理器只能返回 `CPromiseResult`，里面起的链只能是旁支；要参与当前链必须 `ThenPromise`
（同上下文直接把子链返回即可；**跨上下文**先 `CPromise::New` 桥接）。

### 2.4 错误是错误码，不是异常对象

JS 用 `reject(Error)`，可以带 message / stack；本框架用 `int` 码（业务码从 `kBusinessBase` 起），
框架只解释 4 个保留码：

| 码 | 含义 |
|---|---|
| `kFulfilled = 0` | 兑现 —— **注意 `Reject(0)` 等于兑现**，别拿 0 当错误码 |
| `kRejected = 1` | 拒绝（未指定码时的默认值） |
| `kStopped = 2` | 执行器已停止 / 投递失败 |
| `kException = 3` | 处理器或 executor 抛了异常（框架捕获并转成拒绝，不会向调用方抛） |

要带上下文就打日志 / 记到共享上下文里；跨模块时在桥接层把对方的码翻译成本模块的业务码。

### 2.5 线程模型不同：没有事件循环，回调可能跑在别人的线程上

| | JavaScript | 本框架 |
|---|---|---|
| 执行模型 | 单线程事件循环 + 微任务队列 | 多线程执行器（每个模块一个池） |
| 层间推进 | 全部走微任务，顺序有保证且不重入业务栈 | 同一执行器上**内联级联**（省一次入队），深度超限才投递 |
| 跨模块 | 同一个线程，天然无竞争 | 被调模块在自己的池上跑，回调跑在**对方线程**上 |
| 共享数据 | 不需要同步 | 需要原子 / 锁，或约定「一个字段只被一条链写」 |
| 阻塞等待 | 不存在（`await` 不占线程） | `Await()` 会占住一个 worker；线程池被占满时可能死锁 |

实践含义：跨模块的桥接回调里只做「语义转换 + 改上下文 + settle」这类轻活；需要回到本模块线程
就显式投递（`exec.Post(...)`）。**执行器是模块私有资源，不跨模块传递。**

`Await()` 的另一处细节：它返回只表示「这一层已落定」（条件变量被唤醒），
**同一层已登记的 `OnSettled` 回调可能在它之后才跑完** —— 别把 `Await()` 返回当成「所有回调都执行完了」。

### 2.6 未处理的拒绝：JS 会警告，本框架是静默的

JS 里没人 `catch` 的 promise 拒绝会触发 `unhandledrejection`；本框架没有全局兜底，
旁支（fire-and-forget）被拒绝**不会有人知道**。所以旁支要么自己挂 `OnSettled` / `Catch` 记日志，
要么就别开旁支。

### 2.7 其他差异

- **无 `Promise.resolve` / thenable 探测**：跨库、跨回调式 API 的适配要显式写 `CPromise::New`；
- **无 AbortController / 超时**：取消要么在每个层里检查上下文标志，要么用定时器 + `Reject(码)`；
- **`Await()` 之外还有协程**：`CO_AWAIT` 是非阻塞挂起（不占 worker），`Await()` 是阻塞等待；
  生产代码里推荐前者，或干脆全回调（`OnSettled`）。

## 3. 同一段流程的两种写法

```js
// JS：校验 → 查库存（等） → 记录日志（不等） → 落库 → 补偿 → 审计
loadOrder()
  .then(order => validate(order))
  .then(order => queryStock(order))            // 返回 promise → 自动等待
  .then(order => { writeLog(order); return order; })  // 旁支，不等
  .then(order => save(order))
  .catch(err => compensate(err))
  .finally(() => audit());
```

```cpp
// 本框架
return m_exec
    .NewPromise(spCtx, &StepLoad, ASYNC_LOC)        // ① 读订单
    .Then(&StepValidate, ASYNC_LOC)                 // ② 校验
    .ThenPromise(fnQueryStock, ASYNC_LOC)           // ③ 查库存（ThenPromise：等它）
    .Then(&StepWriteLog, ASYNC_LOC)                 // ④ 旁支（普通 Then，不等）
    .Then(&StepSaveOrder, ASYNC_LOC)                // ⑤ 落库
    .Catch(&StepCompensate, ASYNC_LOC)              // catch：仅被拒绝时执行
    .Finally(&StepAudit, ASYNC_LOC);                // finally：成败都跑、不改结果
```

完整可编译版本见 [async-mixed-then-example.md](async-mixed-then-example.md)
（精简版）与 [`examples/cases/ThenMixCase.cpp`](../../examples/cases/ThenMixCase.cpp)（四条路径 + 自校验）。

## 4. 从 JS 迁过来容易踩的坑

| 你的直觉（JS） | 本框架实际 |
|---|---|
| `then` 里 `return` 一个 promise 就会等它 | 必须用 `ThenPromise`；普通 `Then` 里起的链是旁支 |
| `reject(new Error('xx'))` | `Reject(码)`；`Reject(0)` 会被当成兑现 |
| `try/catch` 包住 `await` | `Await()` 不抛异常，返回 `CPromiseResult`；处理器抛异常会被转成 `kException` 拒绝 |
| 回调都在同一个线程，改共享变量不用锁 | 跨模块回调跑在对方线程上，共享数据要原子/锁 |
| `await` 不阻塞线程 | `Await()` 阻塞一个 worker；线程池占满会死锁，纯异步场景请用 `ThenPromise` / `OnSettled` |
| 忘记 catch 会有 `unhandledrejection` | 静默；旁支要自己挂 `OnSettled` |
| 用闭包层层传递值 | 值放进共享上下文；层间只有成败 |
