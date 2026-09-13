# 异步 promise CPromise — 使用文档

> 对应目录：`Common/Async`（命名空间 `common::async`）；协程在 `Common/Coroutine`（见 [coroutine-usage.md](coroutine-usage.md)）
> **链语义**对齐 JS 的 Promise / async-await；但**调度不是 JS 的思路** —— C++ 没有宿主事件循环，
> 所以多出一个显式执行器 `CAsyncExecutor`（JS 里没有对应物），见下表与 [async-vs-js.md §0](async-vs-js.md)。
> 实现细节见：[async-impl.md](async-impl.md) ｜ 协程见：[coroutine-usage.md](coroutine-usage.md)
> 与 JS 的差异对照：[async-vs-js.md](async-vs-js.md) ｜ 单文件示例：[async-mixed-then-example.md](async-mixed-then-example.md)

## 1. JS 对照速查

### 1.1 链语义（JS Promise → 本框架）

| JS | 本框架 |
| --- | --- |
| `new Promise((resolve, reject) => {...})` | `exec.NewPromise(spCtx, 首层)` ⁽*⁾ |
| `new Promise` **由外部回调 settle** | `exec.NewPromise(spCtx, fnStarter)` ⁽*⁾（起链回调里拿到 resolve / reject 句柄） |
| `promise.then(onFulfilled)` | `p.Then(处理器)` |
| `then` 的处理器**返回 promise**（flatten） | `p.ThenPromise(子 promise 工厂)` |
| `then` 里**等另一个模块的 promise 并把数据取回来** | `p.ThenBridge(起子链, 搬数据)`（简写，等价下行两行） |
| `promise.catch(onRejected)` | `p.Catch(处理器)` |
| `promise.finally(onFinally)` | `p.Finally(处理器)` |
| `promise` 已完成 | `p.IsSettled()` |
| `resolve()` / `reject(reason)` | `CPromiseResult::Resolve()` / `CPromiseResult::Reject(码)` |
| `fulfilled` / `rejected` | `result.IsFulfilled()` / `result.IsRejected()` |
| 状态 pending → settled | 每个 then/catch/finally 都返回「指向新一层的 promise」 |
| `Promise.all([a, b])` | `exec.WhenAll(spCtx, a, b)`（详见 §10）；协程内并行也可用 `CO_AWAIT_ALL(a, b)` ⁽*⁾ |
| `Promise.allSettled([a, b])` | `exec.WhenAllSettled(spCtx, a, b)` ⁽*⁾ |
| `Promise.race([a, b])` | `exec.WhenRace(spCtx, a, b)` ⁽*⁾ |
| `Promise.any([a, b])` | `exec.WhenAny(spCtx, a, b)` ⁽*⁾ |

> ⁽*⁾ **结构差异**：JS 的 `new Promise` 是构造函数、`Promise.all` 一族是构造函数上的**静态方法**；
> 本框架挂在**执行器实例**上（`exec.*`），而且多一个 `spCtx` 参数 —— 因为 C++ 里「调度器」与
> 「共享上下文」都必须显式传（JS 靠宿主事件循环与闭包隐式提供）。

### 1.2 本框架有、JS 没有

| 本框架 | 作用 |
| --- | --- |
| `exec.Start()` / `Stop()` / `Post(fn)` | 执行器生命周期与 fire-and-forget 投递（≈ Asio `io_context::post`） |
| `p.Await()` | **阻塞**等待结果（占住 worker，可死锁；≈ C# `Task.Wait()`） |
| `p.AwaitFor(ms)` | 阻塞等待 + 超时（≈ 手写 `Promise.race`） |
| `p.ThenInline()` | 在**结算线程**上就地跑本层（≈ Asio `dispatch` / `ConfigureAwait(false)`） |
| `p.ThenOn(exec)` | 指定执行器的线程上跑本层（≈ `thenApplyAsync(fn, executor)`） |
| `p.OnSettledOn(exec, cb)` | 收尾通知投到指定执行器线程 |
| `exec.CoStart<T>(spCtx)` + `CO_AWAIT` | 无栈协程（≈ C# `Task.Run` + `async/await`） |

## 2. 与「传值版任务链」的区别

本框架**不在层与层之间传递任意值**：

| 维度 | 传值版任务链 | 本框架 |
| --- | --- | --- |
| 层间传什么 | 上一层返回的任意值（类型可变） | 只有兑现 / 拒绝（`CPromiseResult`） |
| 数据怎么传 | 返回值逐层往下递 | 共享上下文 `std::shared_ptr<TContext>` |
| 处理器签名 | 每层不同 | 全部固定 |
| promise 类型 | 随层变化（`CTask<A>` → `CTask<B>`） | 恒为 `CPromise<TContext>` |

固定签名（`then` / `catch` / `finally` 共用）：

```cpp
CPromiseResult handler(CPromiseResult upResult,                  // 上一层的结果
                       const std::shared_ptr<TContext>& spCtx);  // 共享上下文
```

- `upResult`：上一层的结果（起链时恒为「已兑现」）。下一层据此判断上一层；
- `spCtx`：整条 promise 链**共用同一个实例**的数据载体（恒非空）；
- 返回：本层结果。`then` / `catch` 用返回值决定后续走向；`finally` 忽略返回值。

## 3. 最小示例

```cpp
#include "Async/Promise.h"

struct CLoginContext  // 一次流程的共享数据（TContext）
{
    std::string strAccount;
    std::string strToken;
};

common::async::CPromiseResult StepReadParam(common::async::CPromiseResult upResult,
                                            const std::shared_ptr<CLoginContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;  // 上一层被拒绝：原样透传
    }
    spCtx->strAccount = ReadAccount();
    return spCtx->strAccount.empty() ? common::async::CPromiseResult::Reject(kCodeNoAccount)
                                     : common::async::CPromiseResult::Resolve();
}

common::async::CPromiseResult StepRollback(common::async::CPromiseResult upResult,
                                           const std::shared_ptr<CLoginContext>& spCtx)
{
    spCtx->strAccount.clear();  // 仅被拒绝时执行（catch）
    return upResult;            // 透传拒绝；返回 Resolve() 则表示恢复
}

common::async::CAsyncExecutor exec(2);
exec.Start();

std::shared_ptr<CLoginContext> spCtx = std::make_shared<CLoginContext>();
common::async::CPromise<CLoginContext> p = exec.NewPromise(spCtx, StepReadParam, ASYNC_LOC)  // 起 promise（首层）
                                               .Then(StepVerify, ASYNC_LOC)                  // 兑现路径
                                               .Catch(StepRollback, ASYNC_LOC)     // 拒绝路径（可恢复）
                                               .Finally(StepWriteLog, ASYNC_LOC);  // 收尾（兑现 / 拒绝都跑）

p.OnSettled([](common::async::CPromiseResult result) { /* settled 通知 */ });

common::async::CPromiseResult r = p.Await();  // 阻塞取结果
if (r.IsFulfilled())
{
    Use(spCtx->strToken);  // 数据从上下文取
}
```

要点：

- `exec.NewPromise(spCtx, 首层)` **起链即投递首层**（等价 JS 里 `new Promise(executor)` 立即执行 executor）；
- `Then` / `Catch` / `Finally` 各自返回**指向新一层的句柄**；
- 只有最后一层的句柄取结果才有意义 —— 写成 `auto tail = exec.NewPromise(...).Then(...)` 再 `tail.Await()`；
  丢弃返回值时 `p.Await()` 等到的只是首层。

## 4. 三类处理器（then / catch / finally）

| 接口 | 何时执行 | 返回值的作用 |
| --- | --- | --- |
| `Then(handler)` | 上一层**兑现**时；被拒绝则跳过（失败即停） | 决定本层结果（`Resolve()` / `Reject()` / 原样 `upResult`） |
| `Catch(handler)` | 上一层**被拒绝**时；已兑现则跳过 | 同上：返回 `Resolve()` 即**吞掉拒绝**，promise 从本层之后继续 |
| `Finally(handler)` | **无论兑现或拒绝都执行** | **被忽略**，原样透传上一层结果（与 JS `finally` 一致） |

> **then 处理器不需要判断 `upResult`**：失败即停由框架保证 —— 上游被拒绝时本层**根本不会被调用**
> （框架把拒绝结果直接交给下一层）。所以 then 处理器里写
> `if (upResult.IsRejected()) { return upResult; }` 是**永不触发的防御写法**（写了无害，但容易让人
> 误以为「失败也会进来」）；要在拒绝时做事请用 `Catch`，要成败都收尾请用 `Finally`
> —— 这两个处理器的 `upResult` 才有可能是拒绝。
>
> 需要「handler 能被 then / catch / finally 复用」或「独立成可测单元」时，再保留那句判断。

```cpp
// 失败即停：StepStore 被拒绝 → 后续 then 不执行，拒绝码透传
auto r = exec.NewPromise(spCtx, StepReadParam).Then(StepStore).Then(StepNotify).Await();
// r.IsRejected() == true，StepNotify 未执行

// 回滚：catch 看得到拒绝结果
auto t = exec.NewPromise(spCtx, StepLoad)
             .Then(StepStore)
             .Catch(StepRollback)  // 仅被拒绝时执行
             .Await();

// 收尾：finally 无论成败都执行，且不改结果
auto t2 = exec.NewPromise(spCtx, StepLoad)
              .Finally(StepReleaseLock)  // 兑现 / 拒绝都执行
              .Await();
```

同一层可注册多个 `Then`（分叉），各自独立延续。

## 5. 共享上下文（唯一数据通道）

```cpp
// 上下文由调用方强制传入（框架不代建）：先备好数据，再起链
std::shared_ptr<CMyContext> spCtx = std::make_shared<CMyContext>();
spCtx->strRequestId = GetRequestId();
spCtx->nRetry = 3;  // 所有初始字段都在起链前填好
common::async::CPromise<CMyContext> p = exec.NewPromise(spCtx, StepA, ASYNC_LOC).Then(StepB, ASYNC_LOC);
// 链内各层拿到的恒是同一个实例：p.GetContext() == spCtx
```

约束与建议：

- **上下文必须由调用方传入**（`NewPromise` / `CoStart` 的第一个参数）：
  框架**不做懒创建** —— 于是 `GetContext()` 恒非空、核心不必为「可能还没准备好」加锁，
  `TContext` 也不必可默认构造；
- 处理器拿到的是 `const std::shared_ptr<TContext>&`（借用引用，不增加引用计数）；
  若要留给异步回调使用，自行拷贝该 `shared_ptr` 保活；
- 同一条链的层顺序执行、**不会并发**；跨链共享上下文时并发安全由业务负责。

## 6. 嵌套用法（异步里再起异步）

支持嵌套，写法有六种 —— 选哪种只看一条：**是否允许占住工作线程**。

| 形态 | 写法 | 阻塞？ | 适用 |
| --- | --- | --- | --- |
| 协程内 await（**推荐**） | `CO_AWAIT(NewPromise(StepSub))`、`CO_AWAIT(pChild->AsPromise())`、`CO_AWAIT(exec.NewPromise(spOther, StepX))` | 否（挂起让出线程） | 任何「等一段异步再往下走」的场合 |
| 协程内并行 await | `CO_AWAIT_ALL(a, b, c)` | 否 | 多段异步并行 + 汇聚 |
| **跨模块组合**（不用协程） | `p.ThenPromise(工厂)` + `exec.NewPromise(spCtx, fnStarter)` | 否 | 调用**其他模块 / 另一套上下文**的异步函数，且要拿到完整结果 |
| 层内非阻塞嵌套 | 层里起子 promise，由它的 `OnSettled` 回调接着写上下文 / 起后续 | 否 | 层里「顺手起一段异步」，不关心何时回来 |
| 层内 Post | `exec.Post(重活)` | 否 | fire-and-forget 重活下沉 |
| 层内阻塞等待 | 层里 `sub.Await()` | **是**（占住一个 worker） | 仅当线程池还有空闲 worker（**单线程执行器必死锁**） |

### 6.1 协程内 await（推荐）

```cpp
class CFlow : public common::async::CCoroutine<CDemoContext>
{
   public:
    explicit CFlow(const std::shared_ptr<CDemoContext>& spCtx, common::async::CAsyncExecutor* pExec)
        : common::async::CCoroutine<CDemoContext>(spCtx), m_pExec(pExec), m_spSub()
    {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepReadParam));                               // 同上下文子 promise
        m_spSub = std::make_shared<CSubContext>();                          // 跨 await → 成员变量
        CO_AWAIT(m_pExec->NewPromise(m_spSub, &StepQueryRows, ASYNC_LOC));  // **跨上下文** await
        CO_AWAIT(NewPromise(&StepScale).Then(&StepStore));                  // 多步子 promise
        CO_AWAIT_ALL(NewPromise(&StepA), NewPromise(&StepB));               // 并行
        GetContext()->nScaled += m_spSub->nRows;                            // 恢复后并入
        CO_RETURN_VOID();
        CO_END();
    }

   private:
    common::async::CAsyncExecutor* m_pExec;
    std::shared_ptr<CSubContext> m_spSub;  // 跨 await 的变量必须是成员
};
```

- `CO_AWAIT` / `CO_AWAIT_ALL` 接受**任意上下文类型**的 promise（跨流程 / 跨模块组合）；
- 挂起不占线程，**单线程执行器也能跑**。

### 6.2 层内非阻塞嵌套（回调驱动）

```cpp
exec.NewPromise(spCtx, [&exec, spSub](common::async::CPromiseResult up, const std::shared_ptr<CDemoContext>& sp)
{
    if (up.IsRejected())
    {
        return up;
    }
    exec.NewPromise(spSub, &StepQueryRows, ASYNC_LOC)              // 起子 promise 但不等待
        .OnSettled([sp, spSub](common::async::CPromiseResult sub)  // 子流程结束后接着干活
    {
        sp->nScaled = sub.IsFulfilled() ? spSub->nRows : -1;
    });
    sp->strTrace += "父层起步;";
    return common::async::CPromiseResult::Resolve();  // 外层立刻继续
}, ASYNC_LOC);
```

> **「不等它」= 完成时机不由链保证**：上面的「起子 promise 不等」与层里 `exec.Post(...)`
> 都属于 fire-and-forget —— 框架只保证它**最终会跑完**，不保证它跑在**主链结束之前**
> （线程越多，主链越可能先结束：并行等待会真的并行）。所以要断言 / 依赖它的结果，得自己同步
> （原子标志 + 有上限的等待，见 `examples/main.cpp` 的 `WaitCount`），别写成时序侥幸。

### 6.3 跨模块组合：`ThenBridge`（推荐）/ `ThenPromise` + `exec.NewPromise(spCtx, fnStarter)`

场景：模块 A 的业务流程要调「**模块 B（另一套上下文类型）**」的异步函数，
且模块 A 的调用方希望拿到的 promise 反映**含 B 在内的完整结果**。

#### 推荐写法：`ThenBridge`（起子链 + 搬数据，一行搞定）

```cpp
// ① 起子链：轮到本层时发起跨模块调用（跑在本模块线程上，只发起不干活）
auto fnCreateRows = [deps](const std::shared_ptr<CMyContext>& spSelf) -> common::async::CPromise<COtherCtx>
{
    return deps.spOther->QueryAsync(spSelf->spOtherOp);  // 模块 B 的 promise（另一套上下文）
};

// ② 搬数据：子链兑现时把它的上下文数据搬回本上下文
//    （跑在模块 B 的线程上 → 只搬数据，别碰本模块的其他状态）
auto fnApplyRows = [](const std::shared_ptr<CMyContext>& spSelf, const std::shared_ptr<COtherCtx>& spOther)
{
    spSelf->nRows = spOther->nRows;
};

p = exec.NewPromise(spCtx, &StepValidate, ASYNC_LOC)
        .ThenBridge(fnCreateRows, fnApplyRows, ASYNC_LOC)  // 等模块 B；回来时数据已就位
        .Then(&StepUseRows, ASYNC_LOC)                     // 线程亲和：这一层已回本模块线程
        .Finally(&StepAudit, ASYNC_LOC);
```

`ThenBridge` 的语义（与 `ThenPromise` 完全一致，只是多一步搬数据）：

| 情形 | 行为 |
| --- | --- |
| 子链**兑现** | 先 `fnApply(本上下文, 子上下文)` 搬数据，再兑现本层 |
| 子链**被拒绝** | 本层以**同一拒绝码**被拒绝（不搬数据；后续 `Then` 不执行，`Catch` / `Finally` 仍执行） |
| `fnApply` **抛异常** | 本层以 `kException` 拒绝（异常不会窜出通知回调） |
| 上层被拒绝 | 本层不执行，拒绝原因原样透传 |

- `fnCreate` 在**本链执行器线程**上执行（只做「发起 + 登记回调」）；`fnApply` 在**子链的结算线程**
  （典型：模块 B 的线程）上执行 —— 通知不迁移，所以它只应搬数据；
- 需要因业务规则拒绝（如「库存不足」）时，请在**桥接之后的层**里 `return CPromiseResult::Reject(码)`，
  不要塞进 `fnApply`（它没有返回值，也不该做业务分支）。

#### 等价的手写版：`ThenPromise` + `exec.NewPromise(spCtx, fnStarter)`（`ThenBridge` 内部就是这两步）

两步写出来就是 JS 的组合方式：

```cpp
// ① 桥接：new Promise((resolve, reject) => ...) —— 由模块 B 的完成回调 settle
common::async::CPromise<CMyContext> BridgeQueryOther(const CDeps& deps, const std::shared_ptr<CMyContext>& spCtx)
{
    return deps.spExec->NewPromise(spCtx, [deps, spCtx](const ResolveFn& fnResolve, const RejectFn& fnReject)
    {
        deps.spOther
            ->QueryAsync(spCtx->spOtherOp)  // 模块 B 的 promise（另一套上下文）
            .OnSettled([spCtx, fnResolve, fnReject](common::async::CPromiseResult result)
        {
            // 执行器不可用时框架会就地送达本通知，不必检查返回值
            if (result.IsRejected())
            {
                fnReject(码);
                return;
            }  // 跨模块拒绝码 → 业务码
            spCtx->nRows = spCtx->spOtherOp->nRows;  // 取回数据
            fnResolve();
        });
    }, ASYNC_LOC);
}

// ② 接进本流程：then 的 promise 版（等价 JS 的 then 返回 promise 时自动等待）
p = exec.NewPromise(spCtx, &StepValidate, ASYNC_LOC)
        .ThenPromise(
            [deps](const std::shared_ptr<CMyContext>& sp)
{
    return BridgeQueryOther(deps, sp);
}, ASYNC_LOC)
        .Then(&StepUseRows, ASYNC_LOC)
        .Finally(&StepAudit, ASYNC_LOC);
```

要点：

- **非阻塞**：只登记回调，不占任何 worker（两个模块的线程池互不占用，单线程执行器也安全）；
- **执行器归属**：执行器是模块的私有资源（随模块 Start / Stop），**不要跨模块传递** ——
  跨模块接口只交换 promise + 上下文；被调模块在自己的线程池上跑，调用方拿到的只是它的 promise。
  顺序由「依赖边」保证（内层 settle → 桥接回调 → 本层 settle → 外层下一层，之间有 happens-before），
  **不依赖共享线程**；唯一不保证先后的是 fire-and-forget 旁支。
  参考 `examples/cases/ThenMixCase.cpp`：库存 / 记账模块各持一个执行器；
- **跨模块返回后那一层恒回本模块线程**（线程亲和，2026-09-11 改进 A）：本链的层只在本链执行器的线程上跑
  —— 被调模块 settle 本链时，这一层会被投递回本模块执行器（同执行器内仍然就地内联，不多花一次入队）。
  所以回调里可以直接改本模块状态，无需再显式 `exec.Post(...)`（想显式强制也仍然可用）；
  但 **`OnSettled` 通知不迁移**：它仍在结算线程（= 被调模块线程）上触发，只对「层」做亲和；
  `exec.NewPromise(spCtx, fnStarter)` 的起链回调是「发起」语义，仍在调用线程上同步执行。
  背景与实测：见 [async-cross-module-findings.md](async-cross-module-findings.md)；
- **`OnSettled` 保证送达**（2026-09-11 框架修复）：子 promise 已 settled 且它的执行器不可用
  （被调模块已停止 / 拒绝投递）时，通知改为在**调用线程**上就地执行 —— `OnSettled` 现在**没有返回值**
  （登记即生效），漏检不可能再发生，桥接层不会因此永久 pending。
  注意「层」的语义不变：`Then` / `Catch` / `Finally` 在同样情况下仍以 `kStopped` 收口
  （停了的执行器不再跑新层）。背景见 [async-cross-module-findings.md](async-cross-module-findings.md)；
- 子 promise 可以是**任意 promise**：同一 `TContext` 的 then 链**直接返回**就会被 adopt（无需桥接）；
  跨上下文才需要 `exec.NewPromise(spCtx, fnStarter)` 桥接（本节写法）。内层链被拒绝时，拒绝码会作为本层拒绝
  沿**外层链**透传（外层后续 `Then` 不执行，`Catch` / `Finally` 仍执行）；
- `ThenPromise` 的语义与 `Then` 一致（上层被拒绝则本层不执行），差别是**本层等子 promise**：
  子 promise 兑现 → 本层兑现；子 promise 被拒绝 → 本层以**同一拒绝码**被拒绝（`Catch` / `Finally` 仍会执行）；
- `exec.NewPromise(spCtx, fnStarter)` 的起链回调 **立即（同步）执行**（与 JS 一致），只应做「发起 + 登记回调」，
  由回调调 `fnResolve()` / `fnReject(码)`；
- 桥接处是**唯一**做「跨模块拒绝码 → 业务码」语义转换的地方（例如把数据访问层的
  `kDbRowNotFound` 归一化成「兑现 + bFound=false」，把 `kException` 映射成业务码）；
- 工厂 / 回调请**按值捕获依赖**（执行器 `shared_ptr`、接口 `ScopedInterfacePtr`）与上下文，
  不要在回调里捕获模块 `this` —— 回调可能在模块停止后、甚至在**另一个模块的线程**上执行。
  `ServerExample/Module/ExampleAsyncModule.cpp` 是本形态的完整业务示例（业务模块 ↔ 数据访问模块）。

### 6.4 层内阻塞等待（慎用）

```cpp
const common::async::CPromiseResult sub = exec.NewPromise(spSub, &StepQueryRows, ASYNC_LOC).Await();  // 占住一个 worker
```

- `Await()` 是阻塞等待，它占着的 worker 无法去跑别的 promise；
- 若线程池已无空闲 worker（单线程执行器、或所有 worker 都在嵌套等待）→ **死锁**；
- 所以只适合「子流程很短 + 并发余量充足」，默认请优先 6.1。

### 6.5 并发写共享上下文

框架只保证「**同一条链**的层顺序执行」。并行 / 嵌套产生的多条链若共用同一份上下文：

- 各分支请只写**不同字段**，或自行加同步；
- 并行分支尤其别同时写同一个 `std::string` / 容器（那是数据竞争）；
- 需要「各分支自有数据」时，给每条分支一份自己的上下文（示例 ㉕ 就是这么做的）。

## 7. 结果与通知

```cpp
common::async::CPromiseResult r = p.Await();  // 阻塞等待本层结果（不抛异常；多线程可同时等）
if (r.IsRejected())
{
    Log(r.Code());  // 错误码（业务码 / 框架码）
}

// settled 通知：兑现 / 拒绝都触发一次（不产生新层、不改变结果）
p.OnSettled([](common::async::CPromiseResult result)
{
    Log(result.Code());
});
```

错误码约定：

```cpp
common::async::kFulfilled = 0           // 已兑现
    common::async::kRejected = 1        // 已拒绝（未指定码时的默认值）
    common::async::kStopped = 2         // 执行器已停止 / 投递失败（框架）
    common::async::kException = 3       // 处理器抛异常（框架捕获）
    common::async::kBusinessBase = 100  // 业务错误码从 100 起取
```

框架只解释 1..99，其余码**原样透传**（语义由业务定义）。处理器抛出的异常会被框架捕获，
转为本层被拒绝（`kException`），不会向调用方抛出。

注意：`Await()` 返回与 `OnSettled` 回调的执行**没有先后保证**，测试里若依赖「回调已跑完」
请另用标志 / 条件变量同步。

### 7.1 带超时的等待（`AwaitFor`）

```cpp
common::async::CPromiseResult r = p.AwaitFor(500);  // 最多等 500ms；超时返回被拒绝（kStopped）
common::async::CPromiseResult r2 = p.AwaitFor(-1);  // 负值 = 无限等待，等价 Await()
```

用途：测试、优雅关闭、启动自检 —— 这些场合**不允许永久挂住**。
超时只向调用方报「没等到」，**不取消也不落定本层**（链继续在后台跑；要停链用执行器 `Stop()`
或业务标记）。另外，在层内 / 本链执行器线程上阻塞等待未落定的层本身就是危险写法，框架会通过
诊断钩子（见 7.3）给出死锁预警。

### 7.2 通知跑在哪条线程（`OnSettled` / `OnSettledOn`）

| 写法 | 通知在哪跑 | 典型用途 |
| --- | --- | --- |
| `OnSettled(handler)` | **结算线程**（典型：被调模块的线程）—— 通知**不迁移** | 只读日志 / 计数 |
| `OnSettledOn(exec, handler)` | **指定执行器**线程（已在该线程则就地，否则投递） | 收尾要碰本模块状态（模块状态只在模块线程上改） |

```cpp
p.OnSettledOn(m_exec, [this](common::async::CPromiseResult r)
{
    m_stat.nFinished += 1;
});  // 回本模块线程收尾
```

送达保证两者一致：执行器不可用时改在结算线程上就地执行，**绝不丢通知**。
通知里抛异常也同样安全：框架捕获 + 通过诊断钩子报告（见 7.3），**不终止进程、不影响链的结果**
（这一点以前是致命的：异常会逃出 worker → `std::terminate`）。

### 7.3 用法诊断钩子（`SetDiagnosticHandler`）

框架会把「不致命但肯定是误用」的情况报告出来，默认策略是 **debug 构建打印 stderr、发布构建忽略**：

```cpp
common::async::SetDiagnosticHandler([](const char* strWhat)
{
    LOG_WARN("async: %s", strWhat);
});                                            // 接日志 / 指标
common::async::SetDiagnosticHandler(nullptr);  // 恢复默认策略
common::async::SetDiagnosticHandler([](const char*)
{
});  // 完全关闭
```

会报告的形态：

| 形态 | 为什么要报 |
| --- | --- |
| 通知里抛异常（`OnSettled` / `OnSettledOn`） | 以前会让异常逃出 worker → 整个进程 `std::terminate`；现在兜住并报告 |
| `exec.Post()` 投递的任务抛异常 | 同上（线程池 worker 本身不捕获异常） |
| `exec.Post(nullptr)` | 空任务：不提交 + 报告（不再「返回 true 却什么也不做」） |
| 层内 / 本链执行器线程上 `Await()` 未落定的层 | 极可能死锁（占住 worker / 卡住本链）—— 改用 `ThenPromise` / 协程 / `AwaitFor(ms)` |

### 7.4 在层里看「我处在哪条链上」（`async/Trace.h`）

同步代码 `backtrace()` 就有调用链，异步里栈早断了。本框架给的是**因果链**：
「本层 ← 谁挂的它 ← …」，可以在**任意层处理器内部**直接取：

```cpp
#include "Async/Trace.h"

static common::async::CPromiseResult StepVerify(common::async::CPromiseResult upResult,
                                                const std::shared_ptr<CMyContext>& spCtx)
{
#if defined(ASYNC_DEBUG_TRACE)   // 整套设施只在调试构建存在（见下）
    common::async::DumpLayerChain();   // 排障：这层是谁挂上来的

    common::async::VisitLayerChain([](const common::async::CLayerInfo& info)
    {
        // DescribeLayer：一层 → 一行富信息（注册点 + 层号/链号 + 线程 + 耗时 + 结果）
        LOG_INFO("%s", common::async::DescribeLayer(info).c_str());
    });
#endif
    return common::async::CPromiseResult::Resolve();
}
```

输出形态（调试构建，近 → 远）：

```text
#0  then    BuildOrderChain  main.cpp:641  链#1 层#3 龄=0ms 本层=0ms 结果=未落定 tid=…  ← 当前层
#1  then    BuildOrderChain  main.cpp:639  链#1 层#2 龄=0ms 本层=0ms 结果=兑现   tid=…
#2  then    BuildOrderChain  main.cpp:637  链#1 层#1 龄=0ms 本层=0ms 结果=兑现   tid=… [链根]
```

| 接口 | 作用 |
| --- | --- |
| `CurrentLayer()` | 当前正在跑的那一层（不在层里 → `nullptr`；**返回 TLS 存储，要留住请拷贝**） |
| `VisitLayerChain(fn)` | 从当前层往上遍历（近 → 远）：给到**视图字段**（`nDepth` / `bCurrent` / `nAgeMs` / `bSettled` / `bFulfilled` / `nCode`）与**层自己的记录**（`loc` / `eMode` / `nLayerId` / `nChainId` / `bChainRoot` / `bSubChain` / `tid` / `nSelfMs`） |
| `DescribeLayer(info)` | 一层 → 一行富信息（写日志 / 测试断言） |
| `DescribeLayerChain()` | 整条链拼成一行（`#0 … <- #1 …`） |
| `DumpLayerChain()` | 直接打印到 stderr |

要注意的（异步的固有性质）：

- **只看得到「当前层 + 上游」**：下游（还没跑的层）是运行期才挂的，看不到；
- **整套设施只在调试构建存在**（开关与 `ASYNC_LOC` 同一个：`ASYNC_DEBUG_TRACE`）：
  发布构建下 `CLayerInfo` 与所有接口**整段不参与编译 —— 不是「空操作版本」**，
  所以要写 trace 的地方得自己包 `#if defined(ASYNC_DEBUG_TRACE)`；
  示例里用 `TRACE_ONLY(stmt)` 宏把它压成一行（release 展开为空，零开销）；
- **编辑器里这段代码发灰**？那是 clangd 按 release 解析了（编译数据库的模式问题）——
  见 [vscode-clangd-format.md §7](../vscode-clangd-format.md)；
- 分叉（同层多个 `Then`）→ 树；组合器（`WhenAll` 一族）→ 多父一子；
- **子链能追回父链**：内层链（`ThenPromise`）/ 跨模块子链（`ThenBridge`）/ 协程起的子链都挂在
  「起它的那一层」下面，从子链里能一路追回父链（层外起的链没有父层）；
- 机制、代价与边界见 [async-impl.md](async-impl.md) §14。

## 8. 执行器

```cpp
common::async::CAsyncExecutor exec(4);   // 4 个工作线程
exec.Start();                            // 启动（未启动时起 promise 立即被拒绝 kStopped）
exec.Post([]() { /* 无返回值任务 */ });  // fire-and-forget（返回是否提交成功）
exec.Stop();                             // 停止并等待已投递任务完成
```

- `Post`：不涉及 promise 的一次性任务（重活下沉 / 事件异步分发）；
  投递的任务里抛异常 → 框架兜住并报告（见 7.3），**不会终止进程**；
  但线程池 `CThreadPool` 本身**不捕获异常**，所以别绕过执行器直接往线程池提交会抛异常的任务；
- 未 `Start()` / 已 `Stop()` 时起 promise、`Post` 都不抛异常，而是被拒绝 / 返回 `false`；
- `Stop()` 之后可再次 `Start()`（重建句柄与线程池，隔离旧任务）。

## 9. 执行线程控制（逐层亲和 + 建链 / 启动分离）

### 9.1 逐层指定执行线程（`ThenInline` / `ThenOn`）

默认（自 2026-09-11 的线程亲和起）：**每一层都在本链执行器线程上执行**。个别层要换个地方跑时：

| 写法 | 本层在哪跑 | 典型用途 |
| --- | --- | --- |
| `Then(handler)`（默认） | 本链执行器线程（同执行器内联；跨模块返回也会被拉回本模块） | 绝大多数业务层 |
| `ThenInline(handler)` | **结算本层的那条线程**上就地跑（不投递） | 跨模块返回后只想做与对方相关的轻活，省一次回本模块的投递 |
| `ThenOn(exec, handler)` | **指定执行器**线程上（已在该线程则就地，否则投递） | 把重活/旁路工作放到本模块的另一个执行器 |

```cpp
exec.NewPromise(spCtx, StepLoad, ASYNC_LOC)
    .ThenPromise(fnCallOtherModule, ASYNC_LOC)
    .ThenInline(&StepUseResultInline, ASYNC_LOC)    // 就地：或许跑在被调模块线程上
    .ThenOn(m_execSide, &StepHeavyWork, ASYNC_LOC)  // 换到本模块的旁路执行器
    .Then(&StepBackOnMain, ASYNC_LOC);              // 默认亲和：切回本链执行器
```

约束：

- `ThenInline` 的层可能跑在**别的模块的线程**上 → 里面不要碰本模块的非线程安全状态；
- `ThenOn` 的执行器须存活到本层执行完毕；指定执行器已 `Stop()` → 本层以 `kStopped` 收口
  （后续层跳过、`Catch` 照常执行）；
- **不要把 `ThenOn` 用来跨模块传执行器**（执行器是模块私有资源，跨模块只交换 promise + 上下文）；
- 两种写法都只影响**那一层**：之后的层仍按默认亲和回本链执行器；内联深度超 `kMaxInlineDepth` 依旧改投递（防爆栈）。

### 9.2 起链只有一种语义：**立即投递首层**

`exec.NewPromise(...)`（两个重载）**一返回就已经把首层投递出去**了（链边跑边搭），
与 JS 的 `new Promise(executor)` 一致 —— 框架**不提供**「延迟启动」（原来那套
`BuildPromise` + `Start()` 已移除，理由见 [async-impl.md](async-impl.md) §5.1）。

若确实需要「构链期间不跑业务代码」（例如先把所有层与依赖准备好再开跑），
用一次 `exec.Post(...)` 把整段构链放到执行器线程上完成即可 —— 这比框架内置一种第二形态更划算：

```cpp
exec.Post([&exec, spCtx]()
{
    // 这一段整体跑在执行器线程上；首层投递出去时，链已经挂完
    common::async::CPromise<COrderCtx> p = exec.NewPromise(spCtx, StepLoad, ASYNC_LOC)
                                               .ThenPromise(fnCallOtherModule, ASYNC_LOC)
                                               .Then(StepAfterBridge, ASYNC_LOC);
    p.OnSettled([](common::async::CPromiseResult r) { /* 收尾 */ });
});
```

## 10. 组合器：并行汇聚（`WhenAll` / `WhenAllSettled` / `WhenRace` / `WhenAny`）

对齐 JS 的 `Promise.all` / `allSettled` / `race` / `any`：把多条子 promise 汇成**一条聚合链**，
之后照常 `Then` / `Catch` / `Finally` / `Await` / `OnSettled`。它们是**执行器**上的起链入口
（与 `exec.NewPromise` 同族），`CPromise` 侧没有成员形态。

| 入口 | 何时兑现 | 何时拒绝 | 一个子 promise 都不给 |
| --- | --- | --- | --- |
| `exec.WhenAll` | 全部子 promise **兑现** | **任一拒绝 → 立即以该拒绝码拒绝**（对齐 JS 及时失败；其余分支跑完，结果被忽略） | 立即兑现 |
| `exec.WhenAllSettled` | 全部子 promise **落定**（恒兑现） | 不会拒绝 | 立即兑现 |
| `exec.WhenRace` | **首个落定者**兑现 | 首个落定者是拒绝 → 以该拒绝码拒绝 | 立即以 `kRejected` 拒绝 |
| `exec.WhenAny` | **首个兑现者**兑现 | 全部拒绝 → 以**首个拒绝码**拒绝 | 立即以 `kRejected` 拒绝 |

共性语义：

- 子 promise **可跨上下文类型**（各自跑在自己的执行器上）；聚合链自己的层跑在**本执行器**上；
- 数量**运行时确定**时传 `std::vector<CPromise<同上下文> >`，可与单个子 promise **混用**；
- 聚合只关心分支**成败、不传值** —— 数据写各自的共享上下文（同上下文时共用一个实例）；
- 全程只登记回调、**不占工作线程**（单线程执行器也安全）；
- 已落定的子 promise 直接计入（**子 promise 恒有效**：句柄只能由起链入口产出）；
- 框架**不取消**分支：收口后落败 / 剩余分支继续跑完（结果被忽略）。

```cpp
// ① 全部兑现才继续（任一拒绝 → 立即失败）
common::async::CPromise<COrderCtx> p = exec.WhenAll(spCtx, pStock, pBilling).Then(StepGather, ASYNC_LOC);

// ② 数量运行时确定：标量 + 列表可混用
std::vector<common::async::CPromise<COrderCtx> > vecChild = BuildChildren(spCtx);
common::async::CPromiseResult r = exec.WhenAllSettled(spCtx, pHead, vecChild).AwaitFor(1000);

// ③ 多副本取「第一个成功的」
common::async::CPromise<COrderCtx> pAny = exec.WhenAny(spCtx, pReplicaA, pReplicaB);

// ④ 主链路 + 备用链路，谁先有结论用谁（拒绝也算结论）
common::async::CPromise<COrderCtx> pRace = exec.WhenRace(spCtx, pPrimary, pBackup);
```

各分支的成败从**子句柄**读：组合器收口时子句柄都已落定，`child.Await()` 立即返回（不阻塞），
也可以事先给子句柄挂 `OnSettled`。

协程里的等价能力是 `CO_AWAIT_ALL`（并行 await，首个拒绝码终止协程）—— 两条路怎么选见
[coroutine-usage.md 第 9 节](coroutine-usage.md)。

## 11. 线程模型与生命周期

| 事实 | 说明 |
| --- | --- |
| 首层 | 由 `NewPromise` / 构造函数投递到执行器，**在工作线程上执行** |
| 后续层 | 上一层 settled 时按**线程亲和**推进：已在本链执行器线程 → 就地级联；否则投递回本链执行器 |
| 跨模块 | 被调模块的层在它自己的执行器上跑；本链的层**恒回本模块执行器**；`OnSettled` 通知仍在结算线程 |
| 逐层覆盖 | `ThenInline`（就地）/ `ThenOn`（指定执行器）——只影响那一层 |
| 单链并发度 | 一条链的层**顺序执行** |
| 深链 | 连续内联超过 `kMaxInlineDepth`（64）改为投递，防递归爆栈 |
| 分叉 | 同一层可注册多个 `Then`，各自独立延续（按序在同一执行器上推进） |
| 生命周期 | 句柄是浅句柄；promise 通过共享句柄引用线程池，**执行器析构后已起的 promise 仍安全跑完** |

## 12. 常见用法速查

```cpp
// 单层
common::async::CPromiseResult r = exec.NewPromise(spCtx, StepOne, ASYNC_LOC).Await();

// 多层（then 失败即停）+ 收尾
auto tail = exec.NewPromise(spCtx, StepA, ASYNC_LOC).Then(StepB, ASYNC_LOC).Then(StepC, ASYNC_LOC);
tail.OnSettled([](common::async::CPromiseResult r) { /* 兑现 / 拒绝 */ });
common::async::CPromiseResult final = tail.Await();

// 回滚 / 恢复
auto t = exec.NewPromise(spCtx, StepA, ASYNC_LOC).Then(StepB, ASYNC_LOC).Catch(StepRollback, ASYNC_LOC);

// 收尾（不改结果）
auto t2 = exec.NewPromise(spCtx, StepA, ASYNC_LOC).Finally(StepAudit, ASYNC_LOC);

// 不允许永久挂住：带超时等 + 在自己线程收尾
common::async::CPromiseResult r2 = t2.AwaitFor(500);  // 超时 → kStopped
t2.OnSettledOn(m_exec, [](common::async::CPromiseResult)
{
});  // 通知投到本模块执行器

// 分叉
common::async::CPromise<Ctx> head = exec.NewPromise(spCtx, StepA, ASYNC_LOC);
common::async::CPromise<Ctx> b1 = head.Then(StepB, ASYNC_LOC);
common::async::CPromise<Ctx> b2 = head.Then(StepC, ASYNC_LOC);

// 并行汇聚（详见 §10）：全部兑现 / 全部落定 / 首个落定 / 首个兑现
common::async::CPromise<Ctx> tAll = exec.WhenAll(spCtx, b1, b2).Then(StepGather, ASYNC_LOC);
common::async::CPromiseResult rAll = exec.WhenAllSettled(spCtx, b1, b2).AwaitFor(500);

// 起链（上下文必传）：先备好数据，再 NewPromise；想「构链期不跑业务代码」用 exec.Post 包一段（§9.2）
common::async::CPromise<Ctx> c1 = exec.NewPromise(spCtx, StepA, ASYNC_LOC);

// 跨模块组合（纯异步、零阻塞）：桥接 + then-promise 接入（详见 6.3）
auto fnCreateOther = [deps](const std::shared_ptr<Ctx>& sp)
{
    return deps.spOther->QueryAsync(sp->nId);
};
auto fnApplyOther = [](const std::shared_ptr<Ctx>& sp, const std::shared_ptr<COtherCtx>& spOther)
{
    sp->nRows = spOther->nRows;
};
common::async::CPromise<Ctx> p =
    exec.NewPromise(spCtx, StepA, ASYNC_LOC).ThenBridge(fnCreateOther, fnApplyOther, ASYNC_LOC).Then(StepB, ASYNC_LOC);
```

## 13. 与旧版（传值版 `CTask`）的迁移对照

| 旧写法（已移除） | 新写法 |
| --- | --- |
| `exec.Submit([]{ return 3; }).Then([](int n){ return n * 2; })` | 数据放上下文：`spCtx->n = 3;`，处理器读改写 |
| `return common::async::None;`（无值终止） | `return common::async::CPromiseResult::Reject(码);` |
| `r.HasValue() / r.Value()` | `r.IsFulfilled() / r.Code()`，数据从 `GetContext()` 取 |
| `OnSuccess / OnNone` | `Then` / `Catch`（统一用 `CPromiseResult` 判断） |
| `Get()` | `Await()` |
| `NOTHROW_LOC` | `ASYNC_LOC` |
| flatMap（层返回 `CTask`） | 同上下文：`ThenPromise`（处理器返回 promise，框架自动等）；跨上下文：`exec.NewPromise(spCtx, fnStarter)` 桥接（见 6.3 / 协程文档） |

## 14. 测试与示例

- 示例：`examples/main.cpp`（28 个演示：then / catch / finally / 分叉 / 深链 / 协程 / **嵌套** / **跨模块组合** / **多种 then 混用**）；
- 单独用例：`examples/cases/ThenMixCase.cpp`（一条链里混用：具名异步函数 / lambda / lambda 内执行其他异步函数「等与不等」）；
- 业务侧完整示例：`ServerExample/Module/ExampleAsyncModule.cpp`（业务模块 ↔ 数据访问模块，纯异步零阻塞）；
- 单元测试（异步共 **105 例**，全量 151 例）：`test_async_smoke.cpp`（17）对外用法逐条冒烟、
  `test_async_chain.cpp`（37）promise 契约 + 协程、`test_async_combine.cpp`（12）组合器、
  `test_async_modules.cpp`（6）+ `test_async_modules_stress.cpp`（8）跨模块与极限、
  `test_async_affinity.cpp`（5）+ `test_async_affinity_override.cpp`（4）线程亲和，
  `test_async_settled_delivery.cpp`（4）通知送达、
  `test_async_robustness.cpp`（6）健壮性与诊断、`test_async_layer_rules.cpp`（2）三态语义白盒、
  `test_async_alloc.cpp`（2）每层分配预算护栏、
  `test_async_trace.cpp`（6）调用链 trace（复杂主链看完整链 / 多层子链跨链祖先路径 /
  并发多链互不串 / 深链与帧栈无残留 / 层里起链的父层 / 层外空操作契约，见 [async-impl.md](async-impl.md) §14）；
- 基准：`Benchmark/cases/ChainCase.cpp`、`CoroutineCase.cpp`、`ResumableCase.cpp`、`StressCase.cpp`；
- 运行：`./build.sh --tests`、`./build/debug/examples`。
