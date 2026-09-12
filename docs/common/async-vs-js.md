# 异步框架 vs JavaScript Promise：语义对照与边界

> **只有「链语义」是按 JS 的 Promise / async-await 设计的**（then / catch / finally / flatten +
> all / allSettled / race / any），这部分可以直接套用已有直觉。
> **调度不是 JS 的思路**：JS 的调度是隐式的（宿主事件循环 + 微任务队列），本框架必须把它显式化成
> `CAsyncExecutor` —— 所以下文把「语义对照」与「JS 没有对应物」分开列。
> 冒烟测试见 [`Tests/test_async_smoke.cpp`](../../Tests/test_async_smoke.cpp)，用法与 API 见
> [async-usage.md](async-usage.md)。

> 更贴切的整体类比是「**JS 的链语义 + C#/Java 的显式调度模型 + 共享上下文**」——三者合起来才在
> C++ 里成立。调度侧的同类物：Java `Executor` / `CompletableFuture`、C# `TaskScheduler` /
> `SynchronizationContext`、Asio `io_context`、iOS `dispatch_queue`。
> 另可参考 Promise/A+ 风格的 C++ 库（async_promise、Async++ 等）：语义一致，差别在
> 「层间传值 vs 共享上下文」与「编译期选层 vs 运行期 thenable 探测」。

## 0. 为什么会有 `exec` 这个对象

JS 里**根本不存在「执行器」这个概念**，因为两件事由语言 / 宿主隐式提供了：

| JS 的隐式前提 | 本框架的显式对应 |
|---|---|
| 闭包能捕获一切，值沿链自然流动 | 显式参数 `spCtx`（`std::shared_ptr<TContext>`） |
| 宿主事件循环 + 微任务队列决定「排到哪、什么时候跑」 | 显式对象 `CAsyncExecutor`（线程池 + 句柄） |
| 单线程 —— 「哪条线程跑回调」不是问题 | 线程亲和：链的每一层都跑在**本链执行器**线程上（§2.5） |

所以 `exec.WhenAll(spCtx, a, b)` 的两个前导参数恰好就是这两条：**在哪条线程上起聚合链** + **哪一份
上下文**。组合器的语义是 JS 的，多出来的参数与接收者是「把隐式前提显式化」的代价。

## 1. 语义对照（JS 的 Promise → 本框架）

| JavaScript | 本框架 |
|---|---|
| `new Promise(executor)` | `exec.NewPromise(spCtx, 首层)` ⁽¹⁾ |
| `new Promise((resolve, reject) => …)`（由外部 settle） | `exec.NewPromise(spCtx, fnStarter)` ⁽¹⁾：起链回调里发起别的模块 / 回调式异步，由回调 `resolve()` / `reject(码)` |
| `p.then(onFulfilled)` | `p.Then(handler)` |
| `onFulfilled` 返回 promise（自动等待） | `p.ThenPromise(factory)`（factory 返回一条子 promise） |
| `p.catch(onRejected)` | `p.Catch(handler)` |
| `p.finally(onFinally)` | `p.Finally(handler)`（链上的一层）；只做旁路观察用 `p.OnSettled(cb)` ⁽²⁾ |
| `resolve()` / `reject(reason)` | `CPromiseResult::Resolve()` / `CPromiseResult::Reject(码)` |
| `fulfilled` / `rejected` | `result.IsFulfilled()` / `result.IsRejected()` |
| `p` 已完成 | `p.IsSettled()` |
| `Promise.all([a, b, c])` | `exec.WhenAll(spCtx, a, b, c)` ⁽¹⁾（全部兑现才继续，任一拒绝立即失败）；协程内也可 `CO_AWAIT_ALL(a, b, c)` |
| `Promise.allSettled([a, b, c])` | `exec.WhenAllSettled(spCtx, ...)` ⁽¹⁾（全部落定即兑现，不看成败） |
| `Promise.race([a, b])` | `exec.WhenRace(spCtx, a, b)` ⁽¹⁾（首个落定者定结果，拒绝也算结论） |
| `Promise.any([a, b])` | `exec.WhenAny(spCtx, a, b)` ⁽¹⁾（首个兑现者定结果，全拒绝才失败） |
| `Promise.resolve(x)` / `Promise.reject(e)` | `CPromiseResult::Resolve()` / `Reject(码)`（结构化的层结果，不是通用工具函数） |
| `async function` | 协程函数（`Common/Coroutine/Coroutine.h`）⁽³⁾，或纯异步的「层函数 + 链」 |

⁽¹⁾ **结构差异**：JS 的 `new Promise` 是**构造函数**、`Promise.all` 是**构造函数上的静态方法**；
本框架这些都挂在**执行器实例**上（`exec.*`）—— 因为「链在哪条线程上跑」必须由调用方选（见 §0）。
⁽²⁾ `OnSettled` 是「结算通知」，JS 里要写 `p.then(cb, cb)` 凑；它**保证送达**（执行器停了也在调用线程就地送达）。
⁽³⁾ JS 的 `async/await` 是引擎语法糖，没有「起协程」这个调用；本框架要显式 `exec.CoStart<T>(spCtx)`。

## 1.1 本框架有、JS 没有对应物

| 本框架 | 作用 | 该对照谁 |
|---|---|---|
| `CAsyncExecutor`（及 `exec.*` 起链入口） | 线程池 + 句柄 + 停启；回答「投到哪、层跑在哪」 | Java `Executor` / C# `TaskScheduler` / Asio `io_context` |
| `exec.Post(fn)` | 投递无返回值任务（fire-and-forget） | Asio `io_context::post` / Java `Executor.execute` |
| `exec.CoStart<T>(spCtx)` + `CO_AWAIT` | 无栈协程（顺序代码 await 多条链） | C# `Task.Run` + `async/await` |
| `p.Await()` | **阻塞**等待结果（占住 worker，可能死锁） | C# `Task.Wait()` / Java `future.get()` |
| `p.AwaitFor(ms)` | 阻塞等待 + 超时（超时返 `kStopped`，不落定本层） | 要手写 `Promise.race` |
| `p.ThenInline()` | 在**结算线程**上就地跑本层（不要求亲和） | Asio `dispatch` / C# `ConfigureAwait(false)` |
| `p.ThenOn(exec)` | 指定执行器的线程上跑本层 | `CompletableFuture.thenApplyAsync(fn, executor)` |
| `p.OnSettledOn(exec, cb)` | 收尾通知投到指定执行器线程 | — |
| `exec.Stop()` + `kStopped` | 优雅关闭；停止后新投递以 `kStopped` 收口 | —— （JS 没有「运行库被关掉」这一态） |

> JS 侧的 `setTimeout(fn, ms)` / `queueMicrotask(fn)` **不是** `exec.Post` 的对应物：它们是宿主 API，
> 且不可控线程；本框架的定时请用 `Common/Timer` 组件。

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
static common::async::CPromiseResult StepLoad(common::async::CPromiseResult /*up*/,
                                              const std::shared_ptr<COrderCtx>& spCtx)
{
    spCtx->nTotal = 12 * spCtx->nQty;
    return common::async::CPromiseResult::Resolve();
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
if (upResult.IsRejected())
{
    return upResult;
}
```

### 2.3 「等子 promise」要显式写 ThenPromise

JS 在 `then` 里 `return` 一个 thenable，引擎自动 flatten；C++ 编译期无法靠返回值判断，所以
「这一层要等一条子 promise」必须用 `ThenPromise`：

```js
p.then(v => fetchStock(v)).then(v => save(v));          // JS：自动等 fetchStock
p.then(v => { fetchStock(v); });                         // JS：不等（旁支）
```

```cpp
p.ThenPromise(
     [&exec](const std::shared_ptr<Ctx>& sp)
{
    return BridgeQuery(exec, sp);
}, ASYNC_LOC)  // 等
    .Then(&StepSave, ASYNC_LOC);

p.Then([&exec](common::async::CPromiseResult, const std::shared_ptr<Ctx>& sp)  // 不等（旁支）
{
    exec.NewPromise(sp, &StepLog, ASYNC_LOC);
    return common::async::CPromiseResult::Resolve();
}, ASYNC_LOC);
```

普通 `Then` 的处理器只能返回 `CPromiseResult`，里面起的链只能是旁支；要参与当前链必须 `ThenPromise`
（同上下文直接把子链返回即可；**跨上下文**先 `exec.NewPromise(spCtx, fnStarter)` 桥接）。

### 2.4 错误是错误码，不是异常对象

JS 用 `reject(Error)`，可以带 message / stack；本框架用 `int` 码（业务码从 `kBusinessBase` 起），
框架只解释 4 个保留码：

| 码 | 含义 |
|---|---|
| `kFulfilled = 0` | 兑现 —— **注意 `Reject(0)` 等于兑现**，别拿 0 当错误码 |
| `kRejected = 1` | 拒绝（未指定码时的默认值） |
| `kStopped = 2` | 执行器已停止 / 投递失败 |
| `kException = 3` | 处理器或起链回调（`ChainStarter`）抛了异常（框架捕获并转成拒绝，不会向调用方抛） |

要带上下文就打日志 / 记到共享上下文里；跨模块时在桥接层把对方的码翻译成本模块的业务码。

### 2.5 线程模型不同：没有事件循环，回调可能跑在别人的线程上

| | JavaScript | 本框架 |
|---|---|---|
| 执行模型 | 单线程事件循环 + 微任务队列 | 多线程执行器（每个模块一个池） |
| 层间推进 | 全部走微任务，顺序有保证且不重入业务栈 | 同一执行器上**内联级联**（省一次入队），跨执行器则投递回本链执行器 |
| 跨模块 | 同一个线程，天然无竞争 | 被调模块在自己的池上跑；**跨模块返回后的层回本模块线程**（线程亲和），`OnSettled` 通知仍在结算线程 |
| 共享数据 | 不需要同步 | 需要原子 / 锁，或约定「一个字段只被一条链写」 |
| 阻塞等待 | 不存在（`await` 不占线程） | `Await()` 会占住一个 worker；线程池被占满时可能死锁 |

实践含义：跨模块的桥接回调（`OnSettled`）仍跑在被调模块线程上，所以里面只做「语义转换 + 改上下文 + settle」
这类轻活；而**本链的层一律回本模块执行器线程**（线程亲和：被调模块 settle 本链时，本层被投递回本模块），
所以层处理器里可以直接操作本模块状态，不必再手动 `exec.Post(...)`。**执行器是模块私有资源，不跨模块传递。**
改进前后的实测对照见 [async-cross-module-findings.md](async-cross-module-findings.md)。

`Await()` 的另一处细节：它返回只表示「这一层已落定」（条件变量被唤醒），
**同一层已登记的 `OnSettled` 回调可能在它之后才跑完** —— 别把 `Await()` 返回当成「所有回调都执行完了」。

### 2.6 未处理的拒绝：JS 会警告，本框架是静默的

JS 里没人 `catch` 的 promise 拒绝会触发 `unhandledrejection`；本框架没有全局兜底，
旁支（fire-and-forget）被拒绝**不会有人知道**。所以旁支要么自己挂 `OnSettled` / `Catch` 记日志，
要么就别开旁支。

### 2.7 其他差异

- **无 `Promise.resolve` / thenable 探测**：跳库、跳回调式 API 的适配要显式写 `exec.NewPromise(spCtx, fnStarter)`；
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
    .NewPromise(spCtx, &StepLoad, ASYNC_LOC)  // ① 读订单
    .Then(&StepValidate, ASYNC_LOC)           // ② 校验
    .ThenPromise(fnQueryStock, ASYNC_LOC)     // ③ 查库存（ThenPromise：等它）
    .Then(&StepWriteLog, ASYNC_LOC)           // ④ 旁支（普通 Then，不等）
    .Then(&StepSaveOrder, ASYNC_LOC)          // ⑤ 落库
    .Catch(&StepCompensate, ASYNC_LOC)        // catch：仅被拒绝时执行
    .Finally(&StepAudit, ASYNC_LOC);          // finally：成败都跑、不改结果
```

完整可编译版本见 [async-mixed-then-example.md](async-mixed-then-example.md)
（精简版）与 [`examples/cases/ThenMixCase.cpp`](../../examples/cases/ThenMixCase.cpp)（四条路径 + 自校验）。

## 4. 从 JS 迁过来容易踩的坑

| 你的直觉（JS） | 本框架实际 |
|---|---|
| `then` 里 `return` 一个 promise 就会等它 | 必须用 `ThenPromise`；普通 `Then` 里起的链是旁支 |
| `reject(new Error('xx'))` | `Reject(码)`；`Reject(0)` 会被当成兑现 |
| `try/catch` 包住 `await` | `Await()` 不抛异常，返回 `CPromiseResult`；处理器抛异常会被转成 `kException` 拒绝 |
| 回调都在同一个线程，改共享变量不用锁 | 本链的层恒在本模块线程（线程亲和），但 `OnSettled` 回调跑在被调模块线程，共享数据要原子/锁 |
| `await` 不阻塞线程 | `Await()` 阻塞一个 worker；线程池占满会死锁，纯异步场景请用 `ThenPromise` / `OnSettled` |
| 忘记 catch 会有 `unhandledrejection` | 静默；旁支要自己挂 `OnSettled` |
| 用闭包层层传递值 | 值放进共享上下文；层间只有成败 |

## 5. 对照示例：内层链怎么写、怎么被外层等待

JS 原版（内层链 `fetchOrders → fetchPayment`，外层 `.then(total => …)` 等它跑完）：

```js
function fetchUser(id)    { return Promise.resolve({ id, name: 'Alice' }); }
function fetchOrders(user){ return Promise.resolve(['order1', 'order2']); }
function fetchPayment(o)  { return Promise.resolve({ total: 99 }); }

fetchUser(123)
  .then(user => {
    // 内层链：fetchOrders → fetchPayment，顺序执行
    return fetchOrders(user)
      .then(orders => {
        console.log('订单:', orders);
        return fetchPayment(orders[0]);   // 返回下一个 Promise
      })
      .then(payment => {
        console.log('支付:', payment);
        return payment.total;
      });
  })
  .then(total => {
    // 等 fetchOrders 和 fetchPayment 都完成后才执行
    console.log('最终总额:', total);
  });
```

本框架写法（同一流程，代码可编译，输出见下）：

```cpp
// 对照 JS：内层链 fetchOrders → fetchPayment，被外层 .then(total => …) 等待
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"

/// 上下文：JS 里沿链流动的值，本框架都放这里（层间只传兑现 / 拒绝码）。
struct CTradeCtx
{
    int nUserId;
    std::string strUserName;
    std::vector<std::string> vecOrders;
    int nTotal;

    CTradeCtx() : nUserId(0), nTotal(0)
    {}
};

/// @brief 交易模块：自持执行器。
class CTradeModule
{
   public:
    CTradeModule() : m_exec(2)
    {
        m_exec.Start();
    }

    /// @brief 等价 JS：fetchUser(123).then(user => { return 内层链 }).then(total => …)
    common::async::CPromise<CTradeCtx> RunAsync(const std::shared_ptr<CTradeCtx>& spCtx)
    {
        // 等价 `return fetchOrders(user).then(…)`：这一层要等内层链，必须用 ThenPromise
        common::async::CPromise<CTradeCtx>::PromiseFactory fnInner = [this](const std::shared_ptr<CTradeCtx>& spSelf)
        {
            return FetchOrdersAsync(spSelf);
        };

        return m_exec
            .NewPromise(spCtx, &StepFetchUser, ASYNC_LOC)  // fetchUser(id)
            .ThenPromise(fnInner, ASYNC_LOC)               // 等内层链跑完
            .Then(&StepPrintTotal, ASYNC_LOC);             // 等价 .then(total => …)
    }

    /// @brief 内层链：等价 JS `return fetchOrders(user).then(…).then(…)`。
    common::async::CPromise<CTradeCtx> FetchOrdersAsync(const std::shared_ptr<CTradeCtx>& spCtx)
    {
        common::async::CPromise<CTradeCtx>::PromiseFactory fnPayment = [this](const std::shared_ptr<CTradeCtx>& spSelf)
        {
            return m_exec.NewPromise(spSelf, &StepFetchPayment, ASYNC_LOC);
        };

        return m_exec.NewPromise(spCtx, &StepFetchOrders, ASYNC_LOC).ThenPromise(fnPayment, ASYNC_LOC);
    }

   private:
    /// ① fetchUser(id)：结果写进上下文
    static common::async::CPromiseResult StepFetchUser(common::async::CPromiseResult /*upResult*/,
                                                       const std::shared_ptr<CTradeCtx>& spCtx)
    {
        spCtx->strUserName = "Alice";
        std::printf("用户: %d %s\n", spCtx->nUserId, spCtx->strUserName.c_str());
        return common::async::CPromiseResult::Resolve();
    }

    /// ② fetchOrders(user)：等价 `.then(orders => …)`
    static common::async::CPromiseResult StepFetchOrders(common::async::CPromiseResult /*upResult*/,
                                                         const std::shared_ptr<CTradeCtx>& spCtx)
    {
        spCtx->vecOrders.push_back("order1");
        spCtx->vecOrders.push_back("order2");
        std::printf("订单:");
        for (const std::string& strOrder : spCtx->vecOrders)
        {
            std::printf(" %s", strOrder.c_str());
        }
        std::printf("\n");
        return common::async::CPromiseResult::Resolve();
    }

    /// ③ fetchPayment(orders[0])：等价 `.then(payment => …)`
    static common::async::CPromiseResult StepFetchPayment(common::async::CPromiseResult /*upResult*/,
                                                          const std::shared_ptr<CTradeCtx>& spCtx)
    {
        spCtx->nTotal = 99;
        std::printf("支付: %d\n", spCtx->nTotal);
        return common::async::CPromiseResult::Resolve();
    }

    /// ④ .then(total => …)：内层链全部完成后才执行
    static common::async::CPromiseResult StepPrintTotal(common::async::CPromiseResult /*upResult*/,
                                                        const std::shared_ptr<CTradeCtx>& spCtx)
    {
        std::printf("最终总额: %d\n", spCtx->nTotal);
        return common::async::CPromiseResult::Resolve();
    }

    common::async::CAsyncExecutor m_exec;  ///< 模块私有执行器。
};

int main()
{
    auto spModule = std::make_shared<CTradeModule>();

    auto spCtx = std::make_shared<CTradeCtx>();
    spCtx->nUserId = 123;  // fetchUser(123)

    const common::async::CPromiseResult result = spModule->RunAsync(spCtx).Await();
    std::printf("结果: %s\n", result.IsFulfilled() ? "兑现" : "拒绝");
    return 0;
}
```

编译运行（先 `./build.sh --debug Common` 生成 `build/debug/libCommon.a`）：

```bash
g++ -std=c++11 -Wall -Wextra -O0 -g -pthread -ICommon nested_chain.cpp build/debug/libCommon.a -o /tmp/nested_chain && /tmp/nested_chain
```

```text
用户: 123 Alice
订单: order1 order2
支付: 99
最终总额: 99
结果: 兑现
```

三点说明：

- `.then(user => { return 内层链 })` 这一层要写成 `ThenPromise(工厂)`：外层等内层链 settle 后再继续，等价 JS 里「回调返回 promise 会被展平」。
- 值不沿链传：`user` / `orders` / `payment` / `total` 都放在共享上下文 `CTradeCtx` 里，层间只传「兑现 / 拒绝码」，所以每层拿到的是 `spCtx`。
- 内层链被拒绝时，`ThenPromise` 那一层以同一拒绝码被拒绝：外层后续 `Then` 跳过，`Catch` / `Finally` 照常执行。
