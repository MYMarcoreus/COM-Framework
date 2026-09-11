# 异步 promise CPromise — 使用文档

> 对应目录：`Common/Async`（命名空间 `common::async`）
> 命名与语义对齐 **JS 的 Promise / async-await**，便于直接套用已有直觉。
> 实现细节见：[async-impl.md](async-impl.md) ｜ 协程见：[coroutine-usage.md](coroutine-usage.md)
> 与 JS 的差异对照：[async-vs-js.md](async-vs-js.md) ｜ 单文件示例：[async-mixed-then-example.md](async-mixed-then-example.md)

## 1. JS 对照速查

| JS | 本框架 |
| --- | --- |
| `new Promise((resolve, reject) => {...})` | `exec.NewPromise(spCtx, 首层)` 或 `CPromise<Ctx> p(exec, spCtx, 首层)` |
| `new Promise` **由外部回调 settle** | `CPromise<Ctx>::New(exec, spCtx, executor)`（executor 里拿到 resolve / reject 句柄） |
| `promise.then(onFulfilled)` | `p.Then(处理器)` |
| `then` 的处理器**返回 promise**（flatten） | `p.ThenPromise(子 promise 工厂)` |
| `promise.catch(onRejected)` | `p.Catch(处理器)` |
| `promise.finally(onFinally)` | `p.Finally(处理器)` |
| `await promise` | `p.Await()`（阻塞） |
| `promise` 已完成 | `p.IsSettled()` |
| `resolve()` / `reject(reason)` | `CPromiseResult::Resolve()` / `CPromiseResult::Reject(码)` |
| `fulfilled` / `rejected` | `result.IsFulfilled()` / `result.IsRejected()` |
| 状态 pending → settled | 每个 then/catch/finally 都返回「指向新一层的 promise」 |
| `Promise.all([a, b])` | 协程的 `CO_AWAIT_ALL(a, b)` |

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
CPromiseResult handler(CPromiseResult upResult,              // 上一层的结果
                       const std::shared_ptr<TContext>& spCtx); // 共享上下文
```

- `upResult`：上一层的结果（起链时恒为「已兑现」）。下一层据此判断上一层；
- `spCtx`：整条 promise 链**共用同一个实例**的数据载体（恒非空）；
- 返回：本层结果。`then` / `catch` 用返回值决定后续走向；`finally` 忽略返回值。

## 3. 最小示例

```cpp
#include "Async/Promise.h"

namespace no = common::async;

struct CLoginContext                        // 一次流程的共享数据（TContext）
{
    std::string strAccount;
    std::string strToken;
};

no::CPromiseResult StepReadParam(no::CPromiseResult upResult, const std::shared_ptr<CLoginContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;                    // 上一层被拒绝：原样透传
    }
    spCtx->strAccount = ReadAccount();
    return spCtx->strAccount.empty() ? no::CPromiseResult::Reject(kCodeNoAccount)
                                     : no::CPromiseResult::Resolve();
}

no::CPromiseResult StepRollback(no::CPromiseResult upResult, const std::shared_ptr<CLoginContext>& spCtx)
{
    spCtx->strAccount.clear();              // 仅被拒绝时执行（catch）
    return upResult;                        // 透传拒绝；返回 Resolve() 则表示恢复
}

no::CAsyncExecutor exec(2);
exec.Start();

std::shared_ptr<CLoginContext> spCtx = std::make_shared<CLoginContext>();
no::CPromise<CLoginContext> p =
    exec.NewPromise(spCtx, StepReadParam, ASYNC_LOC)   // 起 promise（首层）
        .Then(StepVerify, ASYNC_LOC)                   // 兑现路径
        .Catch(StepRollback, ASYNC_LOC)                // 拒绝路径（可恢复）
        .Finally(StepWriteLog, ASYNC_LOC);             // 收尾（兑现 / 拒绝都跑）

p.OnSettled([](no::CPromiseResult result) { /* settled 通知 */ });

no::CPromiseResult r = p.Await();                      // 阻塞取结果
if (r.IsFulfilled())
{
    Use(spCtx->strToken);                              // 数据从上下文取
}
```

要点：

- `exec.NewPromise(spCtx, 首层)` / `CPromise(exec, spCtx, 首层)` **构造即投递**（等价 `new Promise(executor)` 立即执行 executor）；
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
             .Catch(StepRollback)      // 仅被拒绝时执行
             .Await();

// 收尾：finally 无论成败都执行，且不改结果
auto t2 = exec.NewPromise(spCtx, StepLoad)
              .Finally(StepReleaseLock)   // 兑现 / 拒绝都执行
              .Await();
```

同一层可注册多个 `Then`（分叉），各自独立延续。

## 5. 共享上下文（唯一数据通道）

```cpp
// 方式 A：外部准备数据后注入
std::shared_ptr<CMyContext> spCtx = std::make_shared<CMyContext>();
spCtx->strRequestId = GetRequestId();
no::CPromise<CMyContext> p(exec, spCtx);

// 方式 B：promise 内部懒创建（首次 GetContext() 时构造，恒非空）
no::CPromise<CMyContext> p2(exec);
p2.GetContext()->nRetry = 3;               // 起 promise 前先填数据
p2.Then(StepA).Then(StepB);
```

约束与建议：

- `TContext` 只在真正懒创建（调用 `GetContext()`）时才要求可默认构造；
- 处理器拿到的是 `const std::shared_ptr<TContext>&`（借用引用，不增加引用计数）；
  若要留给异步回调使用，自行拷贝该 `shared_ptr` 保活；
- 同一条链的层顺序执行、**不会并发**；跨链共享上下文时并发安全由业务负责。

## 6. 嵌套用法（异步里再起异步）

支持嵌套，写法有六种 —— 选哪种只看一条：**是否允许占住工作线程**。

| 形态 | 写法 | 阻塞？ | 适用 |
| --- | --- | --- | --- |
| 协程内 await（**推荐**） | `CO_AWAIT(NewPromise(StepSub))`、`CO_AWAIT(pChild->AsPromise())`、`CO_AWAIT(exec.NewPromise(spOther, StepX))` | 否（挂起让出线程） | 任何「等一段异步再往下走」的场合 |
| 协程内并行 await | `CO_AWAIT_ALL(a, b, c)` | 否 | 多段异步并行 + 汇聚 |
| **跨模块组合**（不用协程） | `p.ThenPromise(工厂)` + `CPromise<Ctx>::New(...)` | 否 | 调用**其他模块 / 另一套上下文**的异步函数，且要拿到完整结果 |
| 层内非阻塞嵌套 | 层里起子 promise，由它的 `OnSettled` 回调接着写上下文 / 起后续 | 否 | 层里「顺手起一段异步」，不关心何时回来 |
| 层内 Post | `exec.Post(重活)` | 否 | fire-and-forget 重活下沉 |
| 层内阻塞等待 | 层里 `sub.Await()` | **是**（占住一个 worker） | 仅当线程池还有空闲 worker（**单线程执行器必死锁**） |

### 6.1 协程内 await（推荐）

```cpp
class CFlow : public no::CCoroutine<CDemoContext>
{
public:
    explicit CFlow(const std::shared_ptr<CDemoContext>& spCtx, no::CAsyncExecutor* pExec)
        : no::CCoroutine<CDemoContext>(spCtx), m_pExec(pExec), m_spSub() {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&StepReadParam));                                // 同上下文子 promise
        m_spSub = std::make_shared<CSubContext>();                           // 跨 await → 成员变量
        CO_AWAIT(m_pExec->NewPromise(m_spSub, &StepQueryRows, ASYNC_LOC));   // **跨上下文** await
        CO_AWAIT(NewPromise(&StepScale).Then(&StepStore));                   // 多步子 promise
        CO_AWAIT_ALL(NewPromise(&StepA), NewPromise(&StepB));                // 并行
        GetContext()->nScaled += m_spSub->nRows;                             // 恢复后并入
        CO_RETURN_VOID();
        CO_END();
    }

private:
    no::CAsyncExecutor* m_pExec;
    std::shared_ptr<CSubContext> m_spSub;   // 跨 await 的变量必须是成员
};
```

- `CO_AWAIT` / `CO_AWAIT_ALL` 接受**任意上下文类型**的 promise（跨流程 / 跨模块组合）；
- 挂起不占线程，**单线程执行器也能跑**。

### 6.2 层内非阻塞嵌套（回调驱动）

```cpp
exec.NewPromise(spCtx, [&exec, spSub](no::CPromiseResult up, const std::shared_ptr<CDemoContext>& sp)
{
    if (up.IsRejected()) { return up; }
    exec.NewPromise(spSub, &StepQueryRows, ASYNC_LOC)   // 起子 promise 但不等待
        .OnSettled([sp, spSub](no::CPromiseResult sub)  // 子流程结束后接着干活
        {
            sp->nScaled = sub.IsFulfilled() ? spSub->nRows : -1;
        });
    sp->strTrace += "父层起步;";
    return no::CPromiseResult::Resolve();               // 外层立刻继续
}, ASYNC_LOC);
```

### 6.3 跨模块组合：`ThenPromise` + `CPromise::New`（纯异步、零阻塞、不用协程）

场景：模块 A 的业务流程要调「**模块 B（另一套上下文类型）**」的异步函数，
且模块 A 的调用方希望拿到的 promise 反映**含 B 在内的完整结果**。

两步写出来就是 JS 的组合方式：

```cpp
// ① 桥接：new Promise((resolve, reject) => ...) —— 由模块 B 的完成回调 settle
no::CPromise<CMyContext> BridgeQueryOther(const CDeps& deps, const std::shared_ptr<CMyContext>& spCtx)
{
    return no::CPromise<CMyContext>::New(*deps.spExec, spCtx,
        [deps, spCtx](const ResolveFn& fnResolve, const RejectFn& fnReject)
        {
            deps.spOther->QueryAsync(spCtx->spOtherOp)      // 模块 B 的 promise（另一套上下文）
                .OnSettled([spCtx, fnResolve, fnReject](no::CPromiseResult result)
                {
                    // 执行器不可用时框架会就地送达本通知，不必检查返回值
                    if (result.IsRejected()) { fnReject(码); return; }   // 跨模块拒绝码 → 业务码
                    spCtx->nRows = spCtx->spOtherOp->nRows;             // 取回数据
                    fnResolve();
                });
        }, ASYNC_LOC);
}

// ② 接进本流程：then 的 promise 版（等价 JS 的 then 返回 promise 时自动等待）
p = exec.NewPromise(spCtx, &StepValidate, ASYNC_LOC)
        .ThenPromise([deps](const std::shared_ptr<CMyContext>& sp) { return BridgeQueryOther(deps, sp); }, ASYNC_LOC)
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
  `CPromise::New` 的 executor 是「发起」语义，仍在调用线程上同步执行。
  背景与实测：见 [async-cross-module-findings.md](async-cross-module-findings.md)；
- **`OnSettled` 保证送达**（2026-09-11 框架修复）：子 promise 已 settled 且它的执行器不可用
  （被调模块已停止 / 拒绝投递）时，通知改为在**调用线程**上就地执行 —— 调用方**不需要**检查返回值，
  漏检也不会让桥接层永久 pending（返回值只在 promise 无效时为 `false`）。
  注意「层」的语义不变：`Then` / `Catch` / `Finally` 在同样情况下仍以 `kStopped` 收口
  （停了的执行器不再跑新层）。背景见 [async-cross-module-findings.md](async-cross-module-findings.md)；
- 子 promise 可以是**任意 promise**：同一 `TContext` 的 then 链**直接返回**就会被 adopt（无需桥接）；
  跨上下文才需要 `CPromise::New` 桥接（本节写法）。内层链被拒绝时，拒绝码会作为本层拒绝
  沿**外层链**透传（外层后续 `Then` 不执行，`Catch` / `Finally` 仍执行）；
- `ThenPromise` 的语义与 `Then` 一致（上层被拒绝则本层不执行），差别是**本层等子 promise**：
  子 promise 兑现 → 本层兑现；子 promise 被拒绝 → 本层以**同一拒绝码**被拒绝（`Catch` / `Finally` 仍会执行）；
- `CPromise::New` 的 executor **立即（同步）执行**（与 JS 一致），只应做「发起 + 登记回调」，
  由回调调 `fnResolve()` / `fnReject(码)`；
- 桥接处是**唯一**做「跨模块拒绝码 → 业务码」语义转换的地方（例如把数据访问层的
  `kDbRowNotFound` 归一化成「兑现 + bFound=false」，把 `kException` 映射成业务码）；
- 工厂 / 回调请**按值捕获依赖**（执行器 `shared_ptr`、接口 `ScopedInterfacePtr`）与上下文，
  不要在回调里捕获模块 `this` —— 回调可能在模块停止后、甚至在**另一个模块的线程**上执行。
  `ServerExample/Module/ExampleAsyncModule.cpp` 是本形态的完整业务示例（业务模块 ↔ 数据访问模块）。

### 6.4 层内阻塞等待（慎用）

```cpp
const no::CPromiseResult sub = exec.NewPromise(spSub, &StepQueryRows, ASYNC_LOC).Await();  // 占住一个 worker
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
no::CPromiseResult r = p.Await();     // 阻塞等待本层结果（不抛异常；多线程可同时等）
if (r.IsRejected())
{
    Log(r.Code());                    // 错误码（业务码 / 框架码）
}

// settled 通知：兑现 / 拒绝都触发一次（不产生新层、不改变结果）
p.OnSettled([](no::CPromiseResult result) { Log(result.Code()); });
```

错误码约定：

```cpp
no::kFulfilled     = 0    // 已兑现
no::kRejected      = 1    // 已拒绝（未指定码时的默认值）
no::kStopped       = 2    // 执行器已停止 / 投递失败（框架）
no::kException     = 3    // 处理器抛异常（框架捕获）
no::kBusinessBase  = 100  // 业务错误码从 100 起取
```

框架只解释 1..99，其余码**原样透传**（语义由业务定义）。处理器抛出的异常会被框架捕获，
转为本层被拒绝（`kException`），不会向调用方抛出。

注意：`Await()` 返回与 `OnSettled` 回调的执行**没有先后保证**，测试里若依赖「回调已跑完」
请另用标志 / 条件变量同步。

## 8. 执行器

```cpp
no::CAsyncExecutor exec(4);             // 4 个工作线程
exec.Start();                           // 启动（未启动时起 promise 立即被拒绝 kStopped）
exec.Post([]() { /* 无返回值任务 */ });  // fire-and-forget（返回是否提交成功）
exec.IsIdle();                          // 队列是否为空（协程内联续接判断用）
exec.Stop();                            // 停止并等待已投递任务完成
```

- `Post`：不涉及 promise 的一次性任务（重活下沉 / 事件异步分发）；
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
    .ThenInline(&StepUseResultInline, ASYNC_LOC)   // 就地：或许跑在被调模块线程上
    .ThenOn(m_execSide, &StepHeavyWork, ASYNC_LOC) // 换到本模块的旁路执行器
    .Then(&StepBackOnMain, ASYNC_LOC);             // 默认亲和：切回本链执行器
```

约束：

- `ThenInline` 的层可能跑在**别的模块的线程**上 → 里面不要碰本模块的非线程安全状态；
- `ThenOn` 的执行器须存活到本层执行完毕；指定执行器已 `Stop()` → 本层以 `kStopped` 收口
  （后续层跳过、`Catch` 照常执行）；
- **不要把 `ThenOn` 用来跨模块传执行器**（执行器是模块私有资源，跨模块只交换 promise + 上下文）；
- 两种写法都只影响**那一层**：之后的层仍按默认亲和回本链执行器；内联深度超 `kMaxInlineDepth` 依旧改投递（防爆栈）。

### 9.2 先建链、后启动（`BuildPromise` + `Start`）

`NewPromise` 一返回就把首层投递出去了（链边跑边搭）；需要“先把链完全搭好、再开始跑”时用 `BuildPromise`：

```cpp
no::CPromise<COrderCtx> p = exec.BuildPromise(spCtx)
    .Then(StepLoad, ASYNC_LOC)                    // 只登记，不跑
    .ThenPromise(fnCallOtherModule, ASYNC_LOC)     // 跨模块调用此时也没发起
    .Then(StepAfterBridge, ASYNC_LOC);

// 此处可以放心地再改上下文 / 再挂层：没有任何层在跑
p.Start();                     // 此刻才把 StepLoad 投递到执行器
no::CPromiseResult r = p.Await();
```

| 事实 | 说明 |
| --- | --- |
| 构链期 | **不跑任何业务代码**（含 `New(...)` 的发起也不会执行） |
| `Start()` | 投递首层；**幂等**（重复调用无副作用）；普通链（`NewPromise`）调用它无副作用 |
| 漏写 `Start()` | 直接 `Await()` 会**自动启动**（兜底，不会死等） |
| 执行器不可用 | 首层以 `kStopped` 收口（下游继续透传） |
| `Start()` 后追加层 | 仍可用：新层按亲和回本链执行器（“已 settled 后补登”路径） |
| 空链（没挂过任何层） | `Start()` 无动作；`Await()` 仍是既有的“无状态句柄”语义（`kStopped`） |

好处：构链期间可放心初始化上下文；所有层都在首层开跑前登记完毕（跨模块续接不会出现“补登”的时序差异）。

## 10. 线程模型与生命周期

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

## 11. 常见用法速查

```cpp
// 单层
no::CPromiseResult r = exec.NewPromise(spCtx, StepOne, ASYNC_LOC).Await();

// 多层（then 失败即停）+ 收尾
auto tail = exec.NewPromise(spCtx, StepA, ASYNC_LOC).Then(StepB, ASYNC_LOC).Then(StepC, ASYNC_LOC);
tail.OnSettled([](no::CPromiseResult r) { /* 兑现 / 拒绝 */ });
no::CPromiseResult final = tail.Await();

// 回滚 / 恢复
auto t = exec.NewPromise(spCtx, StepA, ASYNC_LOC).Then(StepB, ASYNC_LOC).Catch(StepRollback, ASYNC_LOC);

// 收尾（不改结果）
auto t2 = exec.NewPromise(spCtx, StepA, ASYNC_LOC).Finally(StepAudit, ASYNC_LOC);

// 分叉
no::CPromise<Ctx> head = exec.NewPromise(spCtx, StepA, ASYNC_LOC);
no::CPromise<Ctx> b1 = head.Then(StepB, ASYNC_LOC);
no::CPromise<Ctx> b2 = head.Then(StepC, ASYNC_LOC);

// 惰性上下文 / 外部注入
no::CPromise<Ctx> c1(exec);            // 链内创建
no::CPromise<Ctx> c2(exec, spCtx);     // 外部注入

// 跨模块组合（纯异步、零阻塞）：new Promise 桥接 + then-promise 接入（详见 6.3）
no::CPromise<Ctx> p = exec.NewPromise(spCtx, StepA, ASYNC_LOC)
                         .ThenPromise([deps](const std::shared_ptr<Ctx>& sp) { return BridgeOther(deps, sp); }, ASYNC_LOC)
                         .Then(StepB, ASYNC_LOC);
```

## 12. 与旧版（传值版 `CTask`）的迁移对照

| 旧写法（已移除） | 新写法 |
| --- | --- |
| `exec.Submit([]{ return 3; }).Then([](int n){ return n * 2; })` | 数据放上下文：`spCtx->n = 3;`，处理器读改写 |
| `return no::None;`（无值终止） | `return no::CPromiseResult::Reject(码);` |
| `r.HasValue() / r.Value()` | `r.IsFulfilled() / r.Code()`，数据从 `GetContext()` 取 |
| `OnSuccess / OnNone` | `Then` / `Catch`（统一用 `CPromiseResult` 判断） |
| `Get()` | `Await()` |
| `NOTHROW_LOC` | `ASYNC_LOC` |
| flatMap（层返回 `CTask`） | 同上下文：`ThenPromise`（处理器返回 promise，框架自动等）；跨上下文：`CPromise::New` 桥接（见 6.3 / 协程文档） |

## 13. 测试与示例

- 示例：`examples/main.cpp`（28 个演示：then / catch / finally / 分叉 / 深链 / 协程 / **嵌套** / **跨模块组合** / **多种 then 混用**）；
- 单独用例：`examples/cases/ThenMixCase.cpp`（一条链里混用：具名异步函数 / lambda / lambda 内执行其他异步函数「等与不等」）；
- 业务侧完整示例：`ServerExample/Module/ExampleAsyncModule.cpp`（业务模块 ↔ 数据访问模块，纯异步零阻塞）；
- 单元测试：`Tests/test_async_chain.cpp`（promise 27 例 + 协程 10 例，含跨模块组合 2 例）；
- 基准：`Benchmark/cases/ChainCase.cpp`、`CoroutineCase.cpp`、`ResumableCase.cpp`、`StressCase.cpp`；
- 运行：`./build.sh --tests`、`./build/debug/examples`。
