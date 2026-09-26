# 异步 promise CPromise — 实现文档

> 对应目录：`Common/Async`（命名空间 `common::async`）；协程在 `Common/Coroutine`（见 [coroutine-impl.md](coroutine-impl.md)）
> 使用方式见：[async-usage.md](async-usage.md) ｜ 协程见：[coroutine-impl.md](coroutine-impl.md)

## 1. 总体架构

```text
CAsyncExecutor                 调度层：CThreadPool + 执行器句柄（Start/Stop/Post/NewPromise/WhenAll 一族）
    │  Handle()（shared_ptr<CExecutorHandle>）
    ├── CPromise<TContext>     编排层：承诺状态（每层一个）+ 共享上下文
    │        │
    │        └── CPromiseResult  层结果（层间唯一传递的信息：兑现 / 拒绝）
    └── CCoroutine<TContext>   顺序层：用顺序代码 await 多条 promise（见 coroutine-impl）
```

职责边界（「谁决定什么」）：

- **起链**只在执行器上：`exec.NewPromise(spCtx, 首层, 类别)` / `exec.NewPromise(spCtx, fnStarter, 类别)` /
  `exec.NewPromise(spCtx, …)`（两个重载）/ `exec.CoStart<T>(类别, spCtx)`；`CPromise` 只提供「句柄 + 加层」，没有任何起链入口。
- **调度**（跑在哪条线程：就地内联 / 投递 / 深度限额）在执行器侧：
  `detail::ShouldInline`、`detail::DispatchInlineOrPost`。
- **编排**（层语义：then / catch / finally 三态、失败即停、桥接、通知）在 promise 侧，
  其中 `RunThenHandler` / `RunResultHandler` 只做「造任务体 + 失败收口」，**首层**的「建层 + 强制投递」收在 `CPromise::StartChain` 一处。

文件划分（`Common/Async/`）：

| 文件 | 内容 |
| --- | --- |
| `PromiseResult.h` | `CPromiseResult`（兑现 / 拒绝 + 标准异常，拒绝统一用 `std::exception` 派生对象） |
| `PromiseTypes.h` | `SettledNotice`、`detail::HandlerMode`、`detail::ThenHandler<TContext>`（then：只接上下文）、`detail::ResultHandler<TContext>`（catch / finally：多一个上游结果） |
| `SourceLoc.h` | `CSourceLoc` + `ASYNC_LOC`（注册点调试信息，发布构建零开销） |
| `AsyncExecutor.h/.cpp` | `CAsyncExecutor`、`detail::CExecutorHandle`、`detail::PostToHandle`、`detail::IsInExecutorThread`、`detail::ShouldInline` / `DispatchInlineOrPost`（**调度策略**：跑在哪条线程） |
| `Combine.h` | 组合器（`WhenAll` 一族）与 `detail::Gather*`（聚合状态 / 登记路径 / 参数包摊平） |
| `PromiseState.h` | `detail::CPromiseState`（一层一个状态机：单向开关 + 处理器登记 / 通知路径） |
| `PromiseLayer.h` | `detail::MakeLayerRunner` / `MakeThenRunner` / `MakeResultRunner`（三态语义 + 层体自请过门重入） |
| `PromiseCore.h` | `detail::CPromiseCore<TContext>`（共享上下文 + 执行器句柄 + 派发） |
| `Promise.h` | `CPromise<TContext>`（**编排**：层语义 / 三态 / 桥接）与起链定义 `NewPromise` |
| `Common/Coroutine/Coroutine.h` | `CCoroutine<TContext>` + `CO_*` 宏（**独立目录**：顺序化是另一个关注点，只依赖 `Common/Async`） |
| `Diagnostics.h/.cpp` | 诊断钩子 `DiagnosticHandler` / `SetDiagnosticHandler` / `ReportDiagnostic`（进程级单槽；promise / 协程 / 执行器共用；调试构建默认打印） |
| `Common/Assert.h`（**全框架**） | `ASSERT` / `ASSERT_MSG` + 唯一调试判定 `FRAMEWORK_DEBUG`（见 §13） |

## 2. 为什么固定签名 + 共享上下文

传值版任务链要求框架为每层保存一个值类型结果，并做类型萃取 / 分派（`TaskTraits` /
`RunTransform` 一整套）——链的类型随层数变化，编译期开销与实现复杂度都不小。

本版把「值」移出层间通道：

```text
层间只传：CPromiseResult（16B = shared_ptr<const std::exception>，空 = 兑现；
                       拒绝携带动态异常对象，层间透传零分配）
数据通道：shared_ptr<TContext>（一次流程一个实例，整条链共用）
```

收益：

- promise 只有一种类型 `CPromise<TContext>`，`Then` / `Catch` / `Finally` 返回同类型 → 无需类型萃取；
- 处理器签名统一到**两种固定形状** → 自由函数 / 静态成员 / `bind` / lambda 都能直接注册
  （`then` 一种：只接共享上下文；`catch` / `finally` 一种：多一个上游结果）；
- 跨层数据是引用语义，大对象（报文、连接、DB 句柄）不再层层拷贝；
- 兑现与拒绝是同一枚 `CPromiseResult`，配合 JS 命名的 then / catch / finally，语义直白。

代价：层间没有「类型安全的显式数据通道」，数据约定靠 `TContext` 的字段约束。

## 3. CExecutorHandle：生命周期加固

```cpp
struct CExecutorHandle
{
    std::shared_ptr<common::thread::CThreadPool> m_pPool;  // 线程池（共享持有）
    std::atomic<bool> m_bStopped;                          // 是否已停止（拒绝新投递）
};
```

- `CAsyncExecutor` 持有该句柄；promise / 协程各自持一份 `shared_ptr`；
- 执行器析构 → `Stop()` → 置 `m_bStopped` 并 `pool->Stop()`（等待已投递任务完成）；
  句柄仍被 promise 持有 → 线程池对象**不会悬垂**，已起的 promise 照常跑完；
- 之后的新投递被 `m_bStopped` 拒绝 → 对应层以「执行器已停」被拒绝。

`detail::PostToHandle(handle, fn)` 是唯一投递入口：句柄空 / 已停止 / 池拒绝都返回 `false`，
调用方据此把结果置为 `Reject(std::runtime_error("执行器已停"))`（不抛异常）。

## 4. CPromiseState：一层的状态机

一条 promise 链由若干**状态**串成，一层一个（对应 JS 中「每个 then 返回的新 promise」）：

```cpp
class CPromiseState
{
    std::mutex m_mutex;
    std::condition_variable m_cv;
    Handler m_handlerInline;             // 第一个处理器（1:1 链的常态，免 vector 缓冲分配）
    std::vector<Handler> m_vecHandlers;  // 第二个起（同层分叉）才用
    std::atomic<bool> m_bSettled;        // 自旋读 + 等待谓词
    CPromiseResult m_result;             // settled 后有效
    CLayerInfo m_trace;                 // trace 记录（注册点 + 上游 + 层号/链号…；仅调试构建）
};
```

| 成员 | 语义 |
| --- | --- |
| `Settle(result)` | 首次生效：锁内置结果与 `m_bSettled`，换出「内联槽 + 处理器列表」，`notify_all` 后在**锁外**按序调用（内联槽在前） |
| `AddHandler(handle, cb)` | pending → 登记返回 true（第一个进内联槽，其余进列表）；已 settled → 投递 `cb` 到执行器异步触发；已 settled 且执行器不可用 → false |
| `Await()` | 先自旋 50μs，再在条件变量上阻塞；`notify_all` 支持多线程等待同一 promise |
| `SetTraceLink/LayerInfo/SetLoc`（仅调试构建） | trace 记录：注册点、上游层、模式、层号/链号（§14） |

三个关键设计：

1. **处理器在锁外调用**：处理器内部可能触发下一层的级联，持锁调用会因重入造成死锁；
2. **已 settled 再注册走投递**：与 JS 一致 —— 注册方（可能是业务线程）不被回调阻塞；
3. **第一个处理器就地存**（`m_handlerInline`）：`Then` 只加一个续接的 1:1 链是最常见的形状，
   用内联槽替代 `std::vector`（其缓冲要单独堆分配）→ 每层少一次堆分配（见 §12）。

## 5. 一次 promise 链的完整生命周期

以 `exec.NewPromise(spCtx, f0).Then(f1, TaskKind::kWrite).Then(f2, TaskKind::kWrite)` 为例：

```text
① NewPromise → StartChain（建核心：spCtx + 句柄；建首层状态 s0；投递 f0，起点结果 Resolve）
                       ↓（工作线程 W）
② f0(Resolve, spCtx) 执行 → s0.Settle(r0)
       ├─ 已登记处理器（f1 那层的 s1）：r0 被拒绝？→ s1.Settle(r0)（失败即停，f1 不执行）
       └─ 已登记的 settled 通知（若有）
                       ↓（仍是线程 W，级联）
③ f1(r0, spCtx) → s1.Settle(r1) → 同路径推进 f2
                       ↓
④ f2 → s2.Settle(r2) → Await() 被唤醒 / OnSettled 触发
```

要点：

- **首层必须投递**（`CPromise::StartChain`：直走 `PostToHandle`，不走亲和分派、不内联）：
  起 promise 的线程不执行任何业务代码；
- **后续层级联**（`RunThenHandler` / `RunResultHandler` → `detail::DispatchInlineOrPost`）：若当前线程已是本链执行器的线程 → 就地内联
  （省一次入队）；否则投递回本链执行器（判定集中在执行器侧的 `detail::ShouldInline`）；
  **否则（跨执行器，比如被调模块 settle 本链）投递回本链执行器** —— 见「线程亲和」；
- **then 失败即停不调用处理器**：续接里 `upResult.IsRejected()` 直接 `Settle(upResult)`；
- **catch / finally 走同一续接，但分派不同**（见下节）。

### 级联内联与深度限制（含线程亲和）

```cpp
auto fnRun = MakeThenRunner(...) / MakeResultRunner(...);  // 执行处理器 + settle 本层
if (IsInExecutorThread(pCore->Handle())   // ① 线程亲和：必须在本链执行器线程上
    && InlineDepth() < kMaxInlineDepth    // ② 深度未超限（线程局部计数 64）
    && Gate()->CanRunInline(eKind))       // ③ 读写门：本线程持着本门「同类」槽位且无人在排队
{
    ++InlineDepth();
    fnRun();
    --InlineDepth();  // 就地执行（省一次投递 + 唤醒；槽位沿用外层任务的，不另占不另还）
}
else
{
    PostToHandle(pCore->Handle(), eKind, std::move(fnRun));  // 过门排队（跨执行器 / 深度超限 / 换类别 / 有人排队）
}
```

`InlineDepth()` 是线程局部计数器，链的级联与协程的内联续接**共用**它，
因此「promise + 协程」混合递归也被同一上限保护；加上①后，深度只在**同一执行器线程内**累加，
跨模块不会涨栈。③ 是 2026-09-19 加读写门时补上的条件：**换类别不能就地**（读任务里的写层
要等别的读者退出 = 等自己，必自死锁），**有人在排队也不能就地**（否则会插队，破坏公平）。

### 5.1 起链只有一种语义：立即投递首层（**延迟启动已移除**）

| 入口 | 首层何时投递 | 追加层 |
| --- | --- | --- |
| `exec.NewPromise(spCtx, 首层处理器, 类别)` | 调用即投递（与 JS 的 `new Promise(executor)` 一致） | 上游未 settle 时登记、已 settle 时投递回本链执行器 |
| `exec.NewPromise(spCtx, fnStarter, 类别)` | 由起链回调里的 `resolve()` / `reject(码)` 决定（起链回调与层体同一套派发：本门同类槽位 → 就地；否则按类别过门投递） | 同上 |

两个重载的类别都只定**首层**：后续每层各自在 `Then` 一族里给（**可省略** —— 缺省 `kDirect` 不过门，需要门保护
时由层体首行的 `ASYNC_GATE_*` 声明；共享核心不存类别 —— 同一条链的层
可以读 / 写 / 直投混排）。

**为什么删掉「延迟启动」（原 `BuildPromise` + `Start()` + `CLaunchState`，2026-09-11 引入，2026-09-12 移除）**：

1. 当初引入它是为了解决「跨模块续接时挂层与执行赛跑」。但**线程亲和**（§8.1）之后这个赛跑已经没有行为差异：
   层**恒在本链执行器**上跑 —— 挂层早于上游 settle 就地/投递到本链执行器，挂晚了走「已 settled → 投递回本链执行器」，
   两条路径的**可见结果完全一样**（只是多一次入队）。延迟启动买到的只剩「少一次投递」。
2. 它唯一独有的能力是「构链期完全不跑业务代码」，而这用一行 `exec.Post(...)` 就能自建：
   ```cpp
   exec.Post(common::async::TaskKind::kWrite, [&exec, spCtx]()
   {
       // 整段构链在执行器线程上同步做完；首层投递出去时，链已挂完
       common::async::CPromise<Ctx> p = exec.NewPromise(spCtx, StepA, common::async::TaskKind::kWrite, ASYNC_LOC).Then(StepB, common::async::TaskKind::kWrite, ASYNC_LOC);
       p.OnSettled(...);
   });
   ```
3. 它让**同一个类型承担两种启动模式**：`CPromiseCore` 要带 `m_bDeferred` 原子 + `m_pLaunch` 载荷，
   `Append` / `ThenPromise` / `WaitInternal` / `IsStarted` 都要各留一条「延迟链」分支，
   而 `Start()` 还需一个 `Await()` 自动兜底的隐式行为 —— 合计约 110 行代码与若干分支，
   换来的只是「少一次投递」。按「**能用编译期/单一语义表达的，就不要留成运行时的分支持久态**」
   （§10 第 9、10 条），这笔交易不划算。

**现在的行为**：`CPromise` 恒「已起链」，没有 `IsDeferred()` / `IsStarted()` / `Start()`；
`Await()` 也不再需要「自动启动」兜底。需要「先搭好再跑」的写法见
[async-usage.md §9.2](async-usage.md)（`exec.Post` 包一段构链）。

后续又顺手把它的**最后一点影子**收干净了（2026-09-12，同一分支）：
句柄的「尚未挂首层」中间态（`m_pState == nullptr`）随它一起消失 —— 现在起链 = 建首层状态 + 投递首层，
是一个原子动作（`CPromise::StartChain`），于是 `Append` / `ThenPromise` 各少一条分支，
`CPromise` 里 5 处空判退化为构造断言，不变式从「或有层」变成**「句柄恒指向一个层」**。

## 6. 处理器模式分派（then / catch / finally）

三态语义（JS 的 then / catch / finally）**没有公共的「按模式分派」函数** —— 模式在各自的
调用点就是常量，跳过与结果归一各写一行在那里（`Tests/test_async_layer_rules.cpp` 从行为上盯住这三条）：

```cpp
// AppendThenLayer：上游被拒绝 → 本层跳过，结果原样交给下一层（then 失败即停）
if (upResult.IsRejected()) { pNextState->Settle(upResult); return; }

// AppendResultLayer：catch 遇到已兑现才跳过（finally 从不跳过 —— 成败都执行）
if (eMode == detail::kModeCatch && upResult.IsFulfilled()) { pNextState->Settle(upResult); return; }

// MakeResultRunner：finally 忽略处理器返回值，原样透传上一层结果
return (eMode == detail::kModeFinally) ? upResult : ownResult;
```

注意 `finally` 在上面两段里出现的位置不同：**跳过判断里没有它**（永不跳过，成败都执行），
**结果归一里才有它**（不改结果，原样透传）。两条合起来才是 JS 的 `finally` 语义 ——
所以它的 `eMode` 只服务后者，别把它塞进跳过判断（那样在 finally 上恒为 false，纯属白判）。

`AppendThenLayer` / `AppendResultLayer` 是 `Then` / `Catch` / `Finally` 各自的实现（两种 handler 形状各一个），
续接处不再写三态判断：

```cpp
// AppendThenLayer：上游被拒绝 → 直接透传（本层不执行，处理器也就看不到上游结果）
[pCore, pNextState, fnHandler](const CPromiseResult& upResult)
{
    if (upResult.IsRejected())
    {
        pNextState->Settle(upResult);
        return;
    }
    pCore->RunThenHandler(pNextState, fnHandler);   // fnHandler(spCtx)
}

// AppendResultLayer（catch / finally）：多把上游结果一路带到处理器里
[pCore, pNextState, fnHandler, eMode](const CPromiseResult& upResult)
{
    if (eMode == detail::kModeCatch && upResult.IsFulfilled())  // finally 从不跳过
    {
        pNextState->Settle(upResult);
        return;
    }
    pCore->RunResultHandler(pNextState, fnHandler, upResult, eMode);   // fnHandler(upResult, spCtx)
}
```

两种处理器各自的任务体也只差一句（`MakeLayerRunner` 是共同外壳：trace 帧 + 异常收口 + settle）：

```cpp
// MakeThenRunner：
return MakeLayerRunner(pState, [spContext, fnHandler]() { return fnHandler(spContext); });

// MakeResultRunner（catch / finally）：
return MakeLayerRunner(pState, [spContext, fnHandler, upResult, eMode]()
    {
        const CPromiseResult ownResult = fnHandler(upResult, spContext);
        return (eMode == kModeFinally) ? upResult : ownResult;  // finally 忽略 ownResult，原样透传
    });
```

- `then` / `catch`：返回值即本层结果 → 决定后续走向（catch 返回 `Resolve()` 即恢复）；
- `finally`：返回值被忽略，原样透传 `upResult`；只有抛异常才会改变结果（异常原样成为本层拒绝），
  与 JS `finally` 语义一致。

**首层固定在链首、且恒以 then 语义执行**：起链时那一层就是`StartChain` 建好的首层（起点结果视为
「已兑现」），因此 `Catch` / `Finally` 永远只会是「追加层」—— 挂在首层之后时，上一层已兑现，
`Catch` 不执行（没有可处理的拒绝）。

### 6.1 跨模块组合的三个原语（`exec.NewPromise(spCtx, fnStarter, 类别)` / `ThenPromise` / `ThenBridge`）

| API | JS 对照 | 实现要点 |
| --- | --- | --- |
| `exec.NewPromise(spCtx, fnStarter, 类别, loc)` | `new Promise((resolve, reject) => …)` | 直接建 `CPromiseState` 并交出 `ResolveFn` / `RejectFn`（内部就是 `pState->Settle(...)`）；起链回调按类别过门（本门同类槽位就地、否则投递），抛异常 → 异常原样成为本层拒绝；`Settle` 幂等，故重复 settle / settle 后异常都安全 |
| `CPromise<T>::ThenPromise(factory, 类别, loc)` | `then(处理器返回 promise)` 的 flatten | 建本层 state，在上游 state 上登记 handler：上游被拒 → 直接透传；上游兑现 → `Adopt()` |
| `CPromise<T>::ThenBridge(fnCreate, fnApply, 类别, loc)` | `then` 里「等别的模块 + 取回数据」 | **上面两个原语的语法糖**：内部就是 `Adopt()` + `New`（改走句柄版 `NewFromHandle`）+ `OnSettled`，多出的只是「子链兑现时先 `fnApply` 搬数据」 |

`Adopt()` 做的事：调 `factory(spCtx)` 拿到子 promise，在**子 promise** 的 `OnSettled` 回调里
`pState->Settle(childResult)` —— 本层的 settle 由子 promise 的结果决定。注意点：

- **不阻塞**：全程只登记回调，不 `Await()`、不占工作线程（单线程执行器也安全）；
- 子 promise 的 settle 线程可能是**另一个模块的执行器线程** → 流程函数请按值捕获依赖与上下文，
  不要捕获本模块 `this`（这样流程是纯函数，任何线程上都安全）；
- 工厂抛异常 → 异常原样成为本层拒绝（工厂**必须**给出子链，没有「返回空」这条路）；
  子 promise 的拒绝**原样**成为本层拒绝（后续 `Then` 不执行，`Catch` / `Finally` 仍执行）；
- **保活**：子 promise 的最后一段由「上一段 handler 捕获下一段」链保活，本层 state 被子 promise
  的 `OnSettled` handler 捕获 —— 即使句柄被丢弃，在途的整条链仍安全跑完；
- **`New` 恒为「立即启动」**：它建的是独立新链，起链回调与层体同一套「就地 / 过门」派发（本门同类槽位 → 就地同步；否则按类别投递）；
  到本层的时机由「轮到该层」保证（它挂在哪一层上，就在哪一层 settle 后才执行）。

`ThenBridge` 与手写版的**等价关系**（也是它的实现）：

```text
ThenBridge(fnCreate, fnApply, loc)
  = ThenPromise([=](spSelf) {
        child = fnCreate(spSelf);                    // ① 工厂：在轮到本层时起子链（必须给出子链）；
                                                    //    按本层类别过门 → 恒在本链执行器线程上
        return NewFromHandle(本链执行器句柄, spSelf, // ② 造一条「由外部 settle」的本上下文 promise
            [=](fnResolve, fnReject) {
                child.OnSettled([=](r) {             // ③ 子链落定 → 搬数据 → 收口（OnSettled 保证送达）
                    if (r.IsRejected()) { fnReject(r); return; }
                    try { fnApply(spSelf, child.GetContext()); }
                    catch (const std::exception& e) { fnReject(CPromiseResult::Reject(std::runtime_error(e.what()))); return; }
                    catch (...) { fnReject(CPromiseResult::Reject(std::runtime_error("处理器异常"))); return; }
                    fnResolve();
                });
            }, loc);
    }, loc)
```

要点：

- **没有新增调度路径**：亲和、送达保证全部沿用 `Adopt` / `New` / `OnSettled`
  既有语义 —— 所以桥接层与手写的完全逐项等价（`Tests/test_async_modules.cpp` 有对照用例）；
- `fnApply` 跑在**子链的结算线程**上（通知不迁移）→ 只搬数据；要拒绝（业务规则）放到桥接之后的层；
- 为什么需要 `NewFromHandle`：工厂里只有「本链执行器**句柄**」（`pCore->Handle()`），
  没有 `CAsyncExecutor&`，故把 `New` 的建 state / 投递逻辑抽成句柄版供两者共用。

### 6.2 组合器（`WhenAll` 一族）的实现

四个入口（`WhenAll` / `WhenAllSettled` / `WhenRace` / `WhenAny`）**只差一个策略位**，共用
`detail::Gather(executor, spContext, ePolicy, child...)`（**没有类别参数**：聚合层是框架簿记层，
固定用 `detail::kKindBookkeeping`（= `kDirect`，不过门）—— 它只被 settle、不跑业务代码）：

- **聚合状态是纯状态**：`detail::CGatherState` 不碰上下文类型（只关心子 promise 的成败），
  所以**跨模块 / 跨上下文类型**的分支能汇到同一个聚合上，无需额外机制；
- **登记路径只有一条**：`detail::BindChildGather` 给每个子 promise 挂 `OnSettled`（恒送达），
  已落定的子 promise 直接计入；
- **参数包摊平**：C++11 的 lambda 捕获列表不能展开参数包，所以先用
  `detail::AppendGatherBindings` 把每个子 promise 变成一个「登记动作」（标量 / `std::vector` 两个重载），
  再由聚合链的 executor 逐个执行；
- **锁内判定、锁外收口**：子 promise 可能在**任意线程**上落定，`OnChildSettled` 持锁更新计数与
  收口标志，`settle` 聚合层则在锁外调用（聚合层的下一层可能就地执行，持锁会死锁）；
- **不取消**：收口后迟到的子 promise 直接被忽略，但它们自己继续跑完。

> 实现位置：`Common/Async/Combine.h`（声明在执行器头 `AsyncExecutor.h`，定义在 `Combine.h`——
> 2026-09-26 拆出）。为什么要拆：组合器要造 `CPromise` 实例，需要它的**完整类型**，而执行器头只能
> **前置声明** `CPromise`（`Promise.h` 反过来 include 执行器头，不能成环）；留在执行器头里时这条
> 依赖靠「模板两段查找 + 调用方 TU 碰巧已 include `Promise.h`」兜住，是个隐式契约。
> 用组合器的 TU 请 include `Async/Combine.h`（它显式 include `AsyncExecutor.h` + `Promise.h`）。

## 7. 两种注册时机

| 注册时刻 | 路径 | 执行线程 |
| --- | --- | --- |
| 上一层**未 settled** | 登记到 `m_vecHandlers`，settle 时被调用 | 由「线程亲和」决定：在本链执行器线程上就地级联，否则投递回本链执行器 |
| 上一层**已 settled** | `AddHandler` 检测到 `m_bSettled` → 投递回调 | 本链执行器的工作线程 |

两者都不阻塞调用方，且都保证「本层只执行一次」；自 2026-09-11 的线程亲和（改进 A）起，
**两者的落点线程一致：都是本链执行器线程**（差别只剩执行时机：立即 vs 入队）。

`OnSettled` 走的是同一条路径（`AddHandler(..., bGuaranteedDelivery = true)`）：
它是「通知」不是「层」，因此带**送达保证** ——
执行器可用时投递（同上表第二种），执行器不可用时（被调模块已停 / 拒绝投递）**在调用线程上就地执行**，
绝不丢弃（否则手写桥接漏检返回值就会让本层永久 pending、上层 `Await()` 死等）。
层处理器仍保持 `AddHandler` 的语义：执行器不可用 → 返回 `false` → 框架以「执行器已停」收口本层。

## 8. 线程模型与不变量

| 不变量 | 保证方式 |
| --- | --- |
| 一条链的层不并发 | 状态只在 `Settle` 时按序触发一次处理器，级联在同一线程推进 |
| 同一状态只 settle 一次 | `Settle` 锁内 `m_bSettled` 判定，后续调用直接返回 |
| 处理器不在起链线程执行 | 首层固定走 `CPromise::StartChain`（强制投递） |
| **每层都在本链执行器线程上** | 线程亲和：层派发先判 `IsInExecutorThread`，不满足就投递回本链执行器 || **跨模块返回的层回本模块** | 同上（被调模块 settle 本链时，本链层不在被调模块线程跑） |
| 回调不持锁 | `Settle` 先换出处理器列表，再锁外调用 |
| 无悬垂 | 句柄 / 状态 / 上下文均为 `shared_ptr`，被续接与句柄共同持有 |
| 深链不爆栈 | `kMaxInlineDepth` 上限 + 改投递（且只在同一执行器线程内累加） |

### 8.1 线程亲和（改进 A，2026-09-11）—— 现在只有一种语义

**每一层（与协程的每一次续跑）都在「它所属链的执行器线程」上跑**：已在该线程 → 就地内联
（省一次入队 + 保序），否则（跨执行器 / 跨模块返回）投递回本链执行器。所以「一层跑在哪条
线程上」看链的起链执行器就够了 —— 跨执行器就是**跨链**（别的模块自持执行器、自己起链，
把结果交回来）。

```cpp
// Common/Thread/ThreadPool：worker 线程打 thread_local 标记
extern / static thread_local const CThreadPool* tl_pCurrentPool;  // WorkerLoop 进入设、退出清
static bool CThreadPool::IsInPoolThread(const CThreadPool* pPool);

// Common/Async/AsyncExecutor.h（detail）
inline bool IsInExecutorThread(const std::shared_ptr<CExecutorHandle>& pHandle)
{
    return pHandle != nullptr && CThreadPool::IsInPoolThread(pHandle->m_pPool.get());
}

// Common/Async/AsyncExecutor.h（detail）：就地还是投递的**唯一**判定
inline bool ShouldInline(const std::shared_ptr<CExecutorHandle>& pExec,
                         bool bRequireIdle = false);  // 已在本线程？ + 深度未超限？(+ 线程池无积压？)
inline bool DispatchInlineOrPost(const std::shared_ptr<CExecutorHandle>& pExec,
                                 std::function<void()> fnTask);  // 就地内联或投递；执行器不可用 → false

// Common/Async/Promise.h：层派发入口（只做「造任务体 + 失败收口」，策略全在执行器侧）
if (!DispatchInlineOrPost(Handle(), std::move(fnRun)))
{
    pState->Settle(CPromiseResult::Reject(std::runtime_error("执行器已停")));  // 执行器不可用 → 本层失败
}
```

- **首层例外**：always 投递（起链线程不跑业务代码）；
- **代价**：每次跨执行器的续接多一次入队 + 唤醒（微秒级）；同执行器内仍完全内联；
  内联深度只在同一执行器线程内累加，跨模块不涨栈；
- **边界**：只作用于「层」——`OnSettled` 通知按登记时的选择在**结算线程**或 `OnSettledOn`
  指定的执行器上送达；`New(...)` 的起链回调是「发起」语义，与层体同一套派发（本门同类槽位就地、否则按类别过门）；
  `Await()` 仍占住调用线程；
- **验收**：`Tests/test_async_affinity.cpp`（5 例，跨模块恒回本模块线程）+ `Tests/test_async_modules*.cpp`
  （当初发现问题的极限用例，现断言 200 条并发链 100% 落回本模块线程）；
- **历史（已移除，2026-09-13，用户要求）**：曾有过「逐层覆盖」（改进 B）—— `ThenInline`
  （`kAffinityInline`，在结算线程上就地）+ `ThenOn(exec, …)`（`kAffinityExecutor`，指定执行器）。
  移除理由：**就地层的落点不可静态判定** —— 注册早于上游落定 → 跟结算线程（跨模块时甚至是别
  的模块的线程）；注册晚于落定 → `AddHandler` 的「已落定 → 投递到本层解析出的执行器」路径把它
  投回**本链执行器**；内联深度超 `kMaxInlineDepth` 又改一次。同一个调用点三种落点，看代码判不出
  线程；而且两个 API 都让层可能跑在**别人的线程**上（碰本模块状态就错），`ThenOn` 还容易被误用成
  「把执行器跨模块传递」。删掉后 `detail::HandlerAffinity` / `ResolveExecHandle` 一并去掉，
  `ShouldInline` / `DispatchInlineOrPost` 只剩一个输入：本链执行器句柄。

分叉（同一状态注册多个 `Then`）时各支线是独立状态：由上游 `Settle` 依次触发，若走
「已 settled 再注册」路径则各自投递 → 各支线依次在同一执行器线程上执行，
但仍共享同一上下文，业务需自行保证上下文字段访问安全。

### 8.2 健壮性：用户回调异常 / 阻塞等待 / 误用诊断（2026-09-12）

一句话：**框架边界上的用户代码一律兜住** —— 异常不让它逃出线程，误用不让它静默，阻塞不让它挂死。

| 场景 | 以前 | 现在 |
| --- | --- | --- |
| 层处理器抛异常 | `MakeLayerRunner` 的 try/catch → 异常原样成为本层拒绝 | 不变（本来就安全） |
| **通知**（`OnSettled` / `OnSettledOn`）抛异常 | 异常从 `CPromiseState::Settle` 逃出 → worker 无 catch → **`std::terminate`（进程挂掉）** | `detail::RunNotice` 兜住 + 报告诊断 |
| `exec.Post(类别, fn)` 的任务抛异常 | 同上（同样能弄死进程） | `CAsyncExecutor::Post` 包一层 guard 兜住 + 报告 |
| `exec.NewPromise(spCtx, starter, 类别)` 的起链回调抛异常 | `RunChainStarter` 兜住 → 异常原样成为拒绝 | 不变 |
| `Await()` 永久挂住 | 只能靠文档警告 | 新增 `AwaitFor(ms)`（超时返回「等待超时」，不落定、不取消链） |
| 层内 / 本链线程上 `Await()` 未落定的层（必死锁） | 无任何提示 | `ReportBlockingRisk()` 报诊断（**不硬失败**：等「别的线程 settle 的层」是合法的） |
| 调用方误用（未传上下文 / 模块未启动 / 对空上下文起链） | 运行期崩在别处，难定位 | `ASSERT` 在开发期直接报位置（见 §13）；无效句柄态已从类型上消除 |

诊断出口（`Async/Diagnostics.cpp`，进程级单槽 + 锁；处理器自身抛异常也会被忽略）：

```cpp
using DiagnosticHandler = std::function<void(const char* strWhat)>;
void SetDiagnosticHandler(const DiagnosticHandler& fnHandler);  // nullptr = 恢复默认
void ReportDiagnostic(const char* strWhat);                     // 框架内部调用
// 默认策略：#if !defined(NDEBUG) → fprintf(stderr, "[async] %s\n", …)；发布构建忽略
```

**边界与不变的约定**：`CThreadPool::WorkerLoop` 依旧**不捕获异常**（「任务自己兜异常」的契约不变，
`ServerCore/Exec` 也是按这个契约自己 catch 的）；async 只在自己这层把**用户回调**包住，
不让框架的用法错误上升成进程级故障。

### 8.3 结算线程：谁 `Settle`，谁决定「本层跑在哪条线程」

「本层跑在结算它的那条线程上」是亲和规则的基础，但**结算线程本身是不确定的**（二选一）：

| 挂层时机 | 本层在哪里跑 |
| --- | --- |
| 上游**未** settle 时挂层（常态） | **结算线程**上（跨模块 = 被调模块的线程；`OnSettled` 通知也在那条线程） |
| 上游**已** settle 后挂层 | 必然投递回**本链执行器**（`AddHandler` 的「已 settled → 投递」路径） |

跨模块桥接会把二者搅在一起：子 promise 若在 `Adopt` 注册通知**之前**就已落定，那条通知会走
「已 settled → 投递」→ 投递回**子 promise 自己的执行器**，于是桥接那一层的结算线程变成调用方自己的执行器，
而不是被调模块的线程。链是「边跑边搭」的，所以这两条路径都可能出现。

结论（写业务与写测试都适用）：

- **不要**把「结算线程是谁」当契约（它取决于子 promise 何时落定）；能确定的只有一件事：
  **本链的层恒在本链执行器线程上**（不论结算线程是谁）—— 要指定的线程请用 `exec.Post(...)`
  （或让子 promise 建在你想要的执行器上）；
- 默认亲和仍提供有用保证：**本链的层恒在本链执行器线程上**（不论结算线程是谁）；
- 测试如果要断言线程：让**用例自己指定结算线程**（自己建子 promise + 在选定执行器上投递结算），
  不要靠固定延时抢时序 —— 踩坑记录见 [async-cross-module-findings.md](async-cross-module-findings.md) 末尾。

### 8.4 读写门：调度侧的准入判定（2026-09-19）

「谁能进入模块」从 2026-09-19 起是调度层的一件事（`Common/Async/ReadWriteGate.h`）：
`CAsyncExecutor` 组合一个 `CReadWriteGate`，挂在**执行器句柄**上（`CExecutorHandle::m_pGate`，
与线程池同寿命 —— promise / 协程持句柄即保活门，执行器析构后门不会悬垂）。
能力来源：`ServerCore/Exec` 的 `CModuleScheduler`（读写调度）整体搬进异步框架后，Exec 目录已删除。

投递漏斗（每个任务都带类别）：

| 路径 | 说法 |
| --- | --- |
| `exec.Post(类别, fn)` | 包一层异常兜底（`kDiagPostThrow`）后按类别过门（`kDirect` 不过门，见下行） |
| 层派发 `PostToHandle(handle, kind, fn)` | `StartChain` 首层、**起链回调**（`NewFromHandle`）、`AddHandler` 的「已落定 → 投递」、`DispatchInlineOrPost` 的投递分支、「等子链」层的动作体（`CPromiseCore::DispatchAction`）—— 按**本层**的类别过门（核心不存类别：同一条链的层可以不同；`NewPromise` 的类别只管首层） |
| 直投层 / `Post(kDirect, fn)` | **直投线程池**（`PostToHandle` 的直投重载）：不入队、不占槽位、不查门 |
| 通知（`OnSettled` / `OnSettledOn`） | **直投**（`PostToHandle(handle, fn)` 的直投重载）：保证送达优先，不去排队等槽位 |
| 就地（`CanRunInline`） | 不过门：槽位已在外层任务手里（同类），只要求「无人在排队」；**直投层不查门**（它不占槽位）—— 在本执行器线程上就接着跑 |

准入算法（与 Exec 时代同款，随能力搬过来）：

- **单一 FIFO 队列 + 队首放行**：读任务在「无写者且未超读上限」时放行，放行后继续看下一个
  （连续读一并放行）；队首是写时等读者排空，其后的读/写一并等待 —— 后来者永远不越过先来者
  （双队列 + 写优先会让「先读后写」执行成「写→读」，这是 Exec 时代踩过的坑）；
- **非阻塞**：进不了就留在队列，线程立即归还线程池；槽位释放时（`OnTaskExit`）继续泵出；
- **异常也归还槽位**：池不捕获异常，门在包装里兜住（`kDiagGateThrow`）后照常归还 ——
  漏归还 = 写者标志回不来 = 模块永久卡死；
- **投递被池拒（停放竞态）时「丢弃 + 报诊断」，不回滚队列**：任务在门口已出队（槽位记在它名下），
  池若已进入停放（`m_bStopping`，只在调用方违反停止顺序时才出现），门归还槽位、报诊断
  `kDiagGateTaskDropped`（与「任务体抛异常」分开）—— 任务体在构造 `Submit` 形参时已被移走
  （拿不回来），而池不会再跑任务（留在队列里也没人会泵出它）；早先那版「回滚回队列」是假保护：
  它只会留下一串空任务，日后被投递时以 `bad_function_call` 伪装成「任务体抛异常」；
- **就地判定用线程局部任务帧**：`detail::CTaskFrame`（自管理：**构造压帧 / 析构弹帧**，与
  `CGateCallScope` / `CCurrentLayerFrame` 同款写法；过门投递的任务压帧，就地跑下来的层沿用外层帧）；
  `CanRunInline` 只看「帧 = 本门 + 同类」且「队列空」；
- **层体自请过门（`ASYNC_GATE`，可选；`Common/Async/GateGuard.h`）**：层体第一行的
  `ASYNC_GATE_READ/WRITE()` 声明「本函数体必须在该类槽位里跑」。已在（含内联级联下来的情况）→ 帧匹配、
  零成本落穿；不在 → 层体返回**占位结果**并在本次调用的作用域（TLS `detail::CGateCallScope`）里登记挂起，
  层运行器**不 settle**、改为把「再跑一遍本层」按请求类别过门投递（重入时帧已匹配 → 那一行自动落穿；
  **不用异常、不用标志位、不新增状态**）。挂起时槽位已归还 → 换类别不会自死锁；本层未 settle → 下游
  不提前跑。**一个函数只允许出现一次**：宏里带固定标签 `ASYNC_GATE_ONCE_PER_FUNCTION`，同一函数里
  第二处 → `duplicate label` **编译错误**；漏网的写法（跨作用域各声明一次、绕过宏直接请求 `EnterGate`）
  由运行时兜底收成**有界失败**（判据：**一次运行只允许挂起一次** —— 第二次挂起即诊断
  `kDiagGateSuspendedTwice` + 本层拒绝），不会在门里来回换档。
  代价：只在「调用点类别 ≠ 函数体声明」时 +1 次过门投递（每层最多一次），正常路径 0 额外分配；
- **`kDirect` 不走这条队列**：直投任务直接投线程池 —— 不占槽位、不受「同类槽位 + 队列空」约束，
  因此也**不受公平性保护**（写任务扎堆时它照样能插进去跑）。契约：不得访问受门保护的数据
  （门对它完全不知情，写者可能正在同时跑）；用途 = 日志 / 指标 / 上报 / 搬运这类自成一体的活。

**哪些用户代码不在门里跑**（三处，刻意的）：

- **起链之前的函数体**（模块公开异步函数里 `NewPromise(...)` 之前那几行）：它是**调用方的代码**、跑在调用方线程上
  （框架还没介入：调用方那扇门里，或者门外）—— 所以公开异步函数体只应做「参数整理 + 起链」，状态访问放进门内首层 / 起链回调；
- **通知**（`OnSettled` / `OnSettledOn`）与 `ThenBridge` 的 `fnApply`：在**子链结算线程**上直投送达
  （通知不迁移），只搬数据；
- **`kDirect` 任务**：直投线程池（见下行）。

其余用户代码**都在门里**，而且跨模块调用时**落在被调模块自己的门里**：

- **起链回调**（`NewPromise(spCtx, fnStarter, 类别)`）：与层体同一套派发（`DispatchInlineOrPost`）——
  本门同类槽位 → 就地同步；门外 / 别的门 / 换类别 → 按类别过门投递。这一条是修出来的：此前起链回调
  恒在调用线程上同步跑，继承的是**调用方那扇门**（跨模块时对**被调模块**的状态零保护；TSan 实测 2 处
  data race）→ `AsyncRw_CrossModuleStarterLandsInCalleeGate` 与 `AsyncRw_StarterRunsUnderDeclaredKind`
  钉住它，`examples/cases/CrossModuleGateCase.cpp` 是可运行的两种写法对照；
- 「等子链」层（`ThenPromise` / `ThenBridge`）的**工厂 `fnCreate`**：走 `CPromiseCore::DispatchAction`
  （就地 / 按本层类别投递），**永远跑在本链执行器线程上**。这一条也是修出来的：之前登记路径直接调
  `Adopt()` 跑工厂，上游若由**外部线程** settle，工厂就在那个线程上、**无槽位**地跑（`CTaskFrame::Top() == NULL`）
  —— `AsyncRw_BridgeFactoryRunsGated` 钉住它（去掉过门那一跳即失败）；
- 协程每一段（`CoStart` / 每个 `CO_AWAIT`）与本链的每一层：各自按本段 / 本层类别过门。

开销（`Tests/test_async_alloc.cpp` 守着）：

- 队列条目与投递目标是**同一个可移动类型** `CWrappedTask`（不是 lambda）—— C++11 的 lambda 不能「移动捕获」，
  包一层会把任务体拷一份，而 `std::function` 的拷贝要再走一次堆分配；
- 放行收集缓冲是**线程局部复用**的（`ScratchDispatchBuffer`），不再每次投递新建一个 vector；
- 合计：每次「过门投递」= 任务包装那 **1 次**分配（池侧已无拷贝：`WorkerLoop` 移动取任务），就地层 0 次；
  `ASYNC_GATE` 同类落穿 0 次，只有「类别升级」那条
  重入路径每层最多再 +1 次（判定全在 TLS / 栈上，零分配）。实测跑链 200 层 **205** 次、
  800 层 **815** 次（≈1.02 次/层；一条链约每 `kMaxInlineDepth`(64) 层才投递一次）。
  （投递路径的完整实测归因与三项优化 —— 过门 864 → 497 ns/任务 —— 见
  [perf-optimization.md](../perf-optimization.md) §4.6。）

**语义边界（必须记住）**：门保证的是「**单个任务**互斥」，不是「**整条链**独占」—— 一条写链的层
与层之间，别的任务（包括别的写）会被放行（队首是谁谁上）。于是：

- 「一个任务内」的读-改-写是安全的（模块状态不需要自己的锁）；
- 「跨层」的读-改-写**不**安全（可能被插队后拿着陈旧数据写回）→ 要么把读改写收进同一个任务，
  要么用乐观锁（读版本 → 带版本写 → 冲突重读重试，`ServerExample` 的数据访问层就是这么做的）。

两条都由 `Tests/test_async_module_threads.cpp` 钉住：前者 = 并发写不丢更新 / 读不撕裂
（`ModuleThreads_ConcurrentWritesKeepExactCount`、`ModuleThreads_ReadersNeverSeeTornState`）；
后者 = `ModuleThreads_WriteChainYieldsBetweenLayers`（两条写链互相等对方的层标记，**确定性**地
证明层间让位：若整链独占，等待就只会超时）+ `ModuleThreads_OptimisticLockKeepsUpdatesExact`
（正确写法：重试到一致，不丢更新）。

停止顺序（`CAsyncExecutor::Stop`）：**关读写门 → 标记停止 → `Drain`（等已接受的跑完）→ 停池**。
先关门再标记，是为了让「看到 `IsStopped()` 为真」的调用方确定「门也已经关了」（门是唯一权威的
准入点，标记只是快速路径）。

门侧的对应契约：`Close` → `Drain` → 停池；反了（或「提交与停池并发」）的后果是任务永远投不出去 ——
投递被拒的那一轮会被**丢弃并报 `kDiagGateTaskDropped`**（不静默、不伪装），队列里剩下的条目则
卡住后续的 `Drain()`：门不提供「违规顺序下也能跑完」的保证。

测试：`Tests/test_async_gate.cpp`（门本体 13 例：读并发 / 写独占 / 三种 FIFO 顺序 / 同门重入 /
多门链式 / 16 门压力 / 排空 / 拒绝路径 / 异常仍归还槽位）+ `Tests/test_async_rw.cpp`
（执行器集成 25 例：写链互斥 / `Post(kRead)` 并发 / 读写不重叠 / 读链并发 / **逐层类别**
（写链里的读层、读链里的写层、各层各自标读、类别在 trace 里可见）/ **`kDirect` 直投不过门**
（写者占门时直投任务与直投链照跑、混排保持链序）/ 组合器聚合层不过门 / 协程逐段类别
（首段与恢复段的任务帧类别各自正确）/ 等子链层的工厂过门（`AsyncRw_BridgeFactoryRunsGated`）/
**起链回调过门**（`AsyncRw_CrossModuleStarterLandsInCalleeGate` 跨模块落在被调模块自己的门里 /
`AsyncRw_StarterRunsUnderDeclaredKind` 换类别时按声明类别进模块）/ **层体自请过门**
（`AsyncRw_GateMacroNoOpWhenKindMatches` 同类落穿 / `AsyncRw_GateMacroUpgradesToWriteSlot` 挂起 + 重入
且真等到读者排空）/ 就地级联同线程 /
`Stop` 排空不丢任务 / 停止中链以「执行器已停」收口）+ `Tests/test_async_gate_macro.cpp`
（`ASYNC_GATE` 专项 14 例：同类就地落穿（同一任务帧）/ 升级写槽位 / 换读槽位 / 兑现与拒绝透传 /
catch 与 finally / 首层 / 同类重复请求落穿（函数形态）/ **第二次挂起即收口**（绕过宏请求两种类别、
辅助 lambda 换档）/与模块自己的写任务零重叠 / 读声明并发）+ `Tests/test_async_module_threads.cpp`
（多线程模块 8 例：并发写不丢更新 / 读不撕裂 / 写链层不重叠 / 层间让位 / 乐观锁重试 / 混合流量
与 `Stop` 后状态自洽 —— 模块状态是**普通成员**，安全全部来自读写门）。

## 9. 源码位置调试（ASYNC_LOC）

```cpp
// Common/Async/SourceLoc.h
#if FRAMEWORK_DEBUG                                     // = 未定义 NDEBUG 且未开优化
    #define ASYNC_DEBUG_TRACE 1
#endif
#define ASYNC_LOC common::async::CSourceLoc(__PRETTY_FUNCTION__, __FILE__, __LINE__)
```

- 调试构建（`-O0`）：每个层状态保存注册点函数名 / 文件 / 行号 —— 调试器里看状态的
  `m_trace.loc`，或用 §14 的 trace 接口（`DescribeLayer`）就能知道「这一层是谁注册的」；
- 发布构建（`-O2`）：`CSourceLoc` 为空位置、**不保存**（`ASYNC_LOC` 展开成空位置），零开销；
- 处理器常是一串 lambda，注册点信息是定位「哪一层被拒绝」的最直接手段；
- 判定收在 [Common/Assert.h](../Common/Assert.h) 的 `FRAMEWORK_DEBUG` 一处（并见
  [assert-usage.md](assert-usage.md)）：`ASYNC_DEBUG_TRACE` 跟着它走 ——
  **注册点、trace 设施、断言三者同一个开关**。

## 10. 设计取舍

1. **eager（起链即投递）**：`NewPromise` / 带处理器的构造函数立即投递首层，
   与 JS 的 `new Promise(executor)` 里 executor 立即执行一致；代价是无法在起链前再改结构。
2. **句柄指向某一层**（而非整条链）：`Then` 返回新句柄，`Await/OnSettled` 作用于句柄所指的状态。
   天然支持分叉、支持「settled 后追加层」，也不必维护「链尾」指针（分叉时链尾不唯一）。
3. **then / catch / finally 三分**：默认安全（`then` 忘写判断也不会在拒绝后误执行后续业务），
   同时给回滚（`Catch`）与收尾（`Finally`）留出与 JS 完全对应的显式出口。
4. **不做取消 / 链级超时**：状态是单向开关，没有取消 API。需要取消时，在业务层用定时器 / 事件
   唤醒后检查标志位；等待侧不挂死由 `AwaitFor(ms)` 兜底（超时返回「等待超时」，**不**取消链）。
5. **上下文为编译期类型**：不提供「向上下文追加任意类型」的容器（如 `std::any`），
   以保证 `TContext` 字段编译期可查、无堆分配、无类型擦除开销。
6. **用户回调的异常在 async 边界收口**（见 §8.2）：层处理器 → 异常原样成为本层拒绝；通知 → 报诊断后忽略；
   `exec.Post` 的任务 → guard 包一层。**不改线程池契约**（`WorkerLoop` 仍不捕获异常）：
   池层吞异常会丢掉「谁抛的」这唯一的线索，而 async 边界知道自己在跑谁的回调、能报告出来。
7. **诊断出口是进程级单槽 + 可替换**（`SetDiagnosticHandler`）：默认 debug 打印 stderr、发布忽略；
   不直接依赖 `Common/Log`（避免低层反向依赖），应用侧一行接入日志 / 指标。
8. **loc / trace 跟着 `FRAMEWORK_DEBUG` 走，不做独立开关**：调试构建自动开启（每层多 16 字节 + 一次
   `SetLoc`），发布构建整段不参与编译 —— 少一个要记住的宏，也不会出现「开了 loc 却没开 trace」
   这类半开状态（两者本来就是同一件事：定位层）。
9. **上下文强制传入，不做懒创建**：`NewPromise(spCtx, …, 类别)` / `CCoroutine(spCtx)` 的上下文参数必传。
   权衡：懒创建能让调用方少写一行 `make_shared`，代价却是——`TContext` 必须可默认构造；
   核心要留 mutable 成员 + mutex；`Context()` 每层多一次空判（热路径）。
   本框架的取舍基准是：**能用编译期约束表达的，就不要留成运行时的分支持久态**。
10. **取消「无效 promise」这一态**：`CPromise` 没有默认构造、没有 `IsValid()`、
    `OnSettled` / `OnSettledOn` 也不再有返回值。
    权衡：默认构造方便了「先声明后赋值」（改成 `shared_ptr` 装句柄即可，样例已改），
    换来的是一整类运行时分支的消失：`Append` / `ThenPromise` 的无效处理、两条诊断文案、
    2 处 `OnSettled` 空判、组合器的「无效子 promise 计「执行器已停」」特例、
    以及调用方到处要写的 `if (!bOk)`。详见 §13。
11. **契约用断言表达，不用运行时宽容**：调用方违约（未传上下文、模块未启动、参数为空）
    在开发期用 `ASSERT` 直接报位置；发布构建下这些断言零开销。
    **业务错误仍走标准异常（拒绝）**，两者不要混。

## 11. 测试与基准

- 单元测试：`Tests/test_async_chain.cpp`
  - promise 22 例：签名契约、顺序与上下文、then 失败即停、catch 观察 / 恢复、finally 不改结果、
    then 与 catch 互补、构造即起链、异常、settled 通知、分叉、settled 后追加、未启动 / 停止 /
    重启、工作线程、析构后完成、并发 Await、多链条并行、深链 300 层、400 条压力、Post 行为；
  - 协程 11 例（含「协程体抛异常 → 本协程拒绝」的收口，见 coroutine-impl.md §4.1）；
- **分配护栏**：`Tests/test_async_alloc.cpp`（3 例，见 §12）——每次改动异步热路径都应让它保持绿；
- 基准：`Benchmark/cases/ChainCase.cpp`（层数 1/5/20/100、深链 256、失败即停）、
  `CoroutineCase.cpp`、`ResumableCase.cpp`、`StressCase.cpp`；
  `Benchmark/results/benchmark-report.md` 由 `./build/release/benchmark` 直接改写，跑完记得一起提交。
- 示例：`examples/main.cpp`；业务侧用法见 `ServerExample/Module/ExampleAsyncModule.cpp`。

## 12. 性能账本（分配预算）

### 为什么盯「分配次数」而不是「锁」

同机微基准（release / `-O2`）：无竞争 `mutex` lock+unlock ≈ **2.4 ns**，而 `make_shared`（136 字节）≈ **15 ns**。
一层的开销几乎全在堆分配上，所以优化目标是**减少分配次数**，而不是先动锁。

### 每层预算（`Tests/test_async_alloc.cpp` 守着它）

| 阶段 | 次数/层 | 内容 |
| --- | --- | --- |
| 建链（`NewPromise` + `Then` × N） | **2** | `make_shared<CPromiseState>`（层状态 176B，含控制块 **192B**）+ 处理器 `std::function`（**72B**） |
| 跑链（`Start` + `Await`） | **1** | 投递给执行器的任务体（`MakeLayerRunner`，**96B**）；就地层不再多花，**过门投递**另加 ≈1 次（读写门包装任务，见 §8.4） |

字节数建链 ≈ **264 字节/层**（release；debug 多一份 `CSourceLoc`），跑链的任务体 96 字节。
另加每链常数 ≤ 8 次分配（核心、延迟载荷、首层 runner 等）。
历史上建链是 **3 次/层**（多一次 `std::vector` 缓冲 32B），现已削到 2 次。

过门投递的摊销：一条链大约每 `kMaxInlineDepth`(64) 层才会「过门投递」一次（其余层在门内就地级联），
所以跑链实测 200 层 **205** 次、800 层 **815** 次（≈1.02 次/层；池侧改移动取任务之前是 213 / 841）；
护栏按 `层数 + 层数/16 + 8` 设上限 —— 比实测宽，但仍能抓住「每层多一次分配」这类回归。

### 读并发的实测与「调度开销边界」（2026-09-19）

`Benchmark/cases/ReadWriteCase.cpp`（256 个任务、每个约 40 µs 业务、4 线程执行器）：

| 行 | 均值 | 相对基线 |
| --- | --- | --- |
| 直接顺序执行同一批工作（基线） | 10.4 ms | 1.0× |
| `Post(kRead)`（读：并发） | **2.7 ms** | **0.3×**（≈3.8×，接近线程数 4） |
| `Post`（写：独占） | 12.1 ms | 1.2×（串行 + 每任务调度开销） |

**边界：任务太短时，测到的是线程池的唤醒成本，而不是读写门。** 池在「队列空窗」下每任务要付
一次睡眠/唤醒（实测 5~15 µs/任务；`strace` 数到约 2 次 `futex`/任务）：把 512 个「空转 64 次」
（≈100 ns 业务）的任务直接投进 4 线程池要 **6~7 ms**（≈12 µs/任务，并发峰值只有 1），而同样
的任务带 40 µs 业务量时读路径能跑满 4 路。也就是说：小任务的瓶颈在池的唤醒策略（`Post` 突发
与读突发都受影响），与准入判定无关 —— 属于可单独优化的点（窗口式持续吞吐见基准报告 §5）。

实际含义：**读并发优化的是「业务量足够大的读」**（几十 µs 以上），这类才是模块里值得并起来的
工作；数百纳秒级的小任务无论读写都被调度开销支配，不要指望读写门在那里制造差异。

上表数字用 `operator new` 插桩实测（请求字节数，release / `-O2`）。与结果「只带码」的那版相比，
结果类型 4B → 24B 使层状态 152 → 176B、任务体 72 → 96B（**分配次数没变**，仍是 2 + 1）；
同机交替微基准（新旧两份库轮流跑 3 轮）显示性能与旧版持平（单链 / 10 层 / 100 层中位数都在 ±2% 内）。

### 已落地的两处优化

1. **上下文强制传入 + 热路径去锁**（`detail::CPromiseCore`）：
   - 上下文由调用方传入（`NewPromise(spCtx, …, 类别)` / `CCoroutine(spCtx)` 都去掉了默认实参）→
     核心**再无可变共享状态**：`Context()` 直接返回成员的 `const` 引用（不加锁、不拷贝 `shared_ptr`）；
     懒创建那一版要 mutable 成员 + mutex + 一个「可能还没准备好」的时间窗，
     而它换来的只是调用方少写一行 `make_shared`（见 §10 第 9 条）；
   - 上下文强制传入、层状态只建一次 —— 核心不再有「是否延迟链」这类分支（延迟启动已移除，见 §5.1）；
2. **handler 内联槽**（`detail::CPromiseState`）：第一个处理器就地存（§4），
   1:1 链每层省掉 `std::vector` 的缓冲分配。

合计效果：建链 **3 → 2 次分配/层**（字节数基本持平 —— 层状态多了 32B 内联槽，正好抵掉 32B 的 vector 缓冲）。

### 实测收益（release，同机三次取中位数，A/B 同一构建脚本）

| 基准项 | 优化前 | 优化后 | 变化 |
| --- | --- | --- | --- |
| `CPromise x100` | 60.50 µs | 55.94 µs | **-7.5%** |
| `CPromise deep x256` | 147.34 µs | 125.22 µs | **-15.0%** |
| `CCoroutine start+await` | 1.19 µs | 1.01 µs | **-15.1%** |
| `CPromise fail-fast x20` | 17.71 µs | 16.29 µs | -8.0% |

链越长收益越明显，因为省下的正是与层数成正比的那一次分配。

### 护栏怎么写的

`Tests/test_async_alloc.cpp` 覆盖全局 `operator new` / `operator delete`（含 `[]`、nothrow、sized 变体），
只在 `CAllocCounter` 的窗口内计数（窗口外只多一次 relaxed 读，不影响其它用例）：

```cpp
CAllocCounter counter;  // 开表
common::async::CPromise<CCtx> tail = exec.NewPromise(spCtx, &StepBump, common::async::TaskKind::kWrite, ASYNC_LOC);
for (int i = 0; i < nLayers; ++i)
{
    tail = tail.Then(&StepBump, TaskKind::kWrite, ASYNC_LOC);
}
counter.Stop();  // 关表
ASSERT_TRUE(counter.Counts() <= 2 * nLayers + 8);
```

两条经验：

- **只设上限，不设下限**：后续把每层做到 1 次（把任务体也塞进层状态）也应照样通过；
- 用 100 层与 500 层的**差值**再断言一次「每多一层 ≤ 2 次」，避免每链常数掩盖线性增长；
- 测量窗口里不能打印 / 构造容器，否则会把无关分配算进去；**在层函数内部测量**建链
  （单线程执行器此刻被本层占用 → 新建链不会立即开跑，窗口里只有建链分配；见
  `Tests/test_async_alloc.cpp` 的 `StepMeasureBuild`）；
  普通起链（`NewPromise`）首层会立刻开跑，worker 的执行分配会掺进窗口，结果不确定。

### 还没做的（按收益排序）

1. **层状态瘦身（P3）**：`mutex` + `condition_variable`（合计 ~120B）在绝大多数层里从未被等待 →
   可挪进「首次 `Await()` 才创建」的等待槽，层状态 330 → ~130 字节/层；
2. **任务体去分配（P4）**：跑链的 1 次/层来自 `std::function<void()>` 任务类型，
   换成 16 字节可调用体（核心指针 + 状态指针）可再去掉一次/层；
3. 取消（`kCancelled` + 令牌沿层 / 组合器 / 协程穿透）、诊断带上注册点 `CSourceLoc`。

## 13. 契约断言（ASSERT）

断言是**全框架通用**设施（`Common/Assert.h`，不只异步用）：用法、开关（`FRAMEWORK_DEBUG`）、
该用 / 不该用的判断标准与完整使用点清单见 [assert-usage.md](assert-usage.md)；
本节只记**异步框架自身的断言点**（设计意图）。

```cpp
#include "Assert.h"

ASSERT(pCore != nullptr);                                        // 内部不变量
ASSERT_MSG(spContext != nullptr, "共享上下文必须由调用方传入");  // 调用前提
```

| 位置 | 断言 | 理由 |
| --- | --- | --- |
| `CPromiseCore` 构造 | `spContext != nullptr` | 上下文强制传入（§10 第 9 条） |
| `CPromise` 私有构造 | `pCore != nullptr` / `pState != nullptr` | 句柄恒有核心、**恒指向一个层**（无「未挂首层」态） |
| `MakeLayerRunner` / `MakeThenRunner` / `MakeResultRunner` / `NewNextLayer` | `spContext` / `pState` 非空 | 内部调用不变量 |
| ~~`CPromise::Start` / `IsStarted` / `RegisterFirstLayer` / `Append`（延迟分支）~~ | ~~延迟链的载荷非空~~ | 延迟启动已移除（§5.1），相应断言一并删除 |
| `CPromiseResult::Reject` | **已无断言** | 失败 = 携带一个标准异常对象，不再有「码 0 = 兑现」的约束；判兑现一律看 `IsFulfilled()` |
| `CCoroutine` 构造 | `spContext != nullptr` | 与 promise 一致 |
| `CCoroutine::AsPromise` / `AwaitWait` / `AwaitEach` | `m_pExec != nullptr` | 必须在 `CoStart` 之后调用 |
| `CExampleDbModule` / `CExampleAsyncModule`（业务侧样例） | 模块已启动、参数非空 | 样例示范「业务契约也用断言钉住」 |

## 14. 异步调用链（trace：在层里看到自己处在哪条链上）

用法见 [async-usage.md §7.4](async-usage.md)；本节记**实现与代价**。

### 为什么不能靠 backtrace

同步代码的调用链就是调用栈；异步里层与层之间是**投递 / 回调**，栈早断了。
所以这里给的是**因果链**：本层 ← 谁挂的它 ← …（一棵 span 树，严格说是 DAG：分叉与组合器）。

### 三个接入点（`Common/Async/Trace.h` / `Trace.cpp`）

| 部件 | 作用 |
| --- | --- |
| `detail::CCurrentLayerFrame` | 跑层时在 **thread_local 上压一帧**；帧对象活在各线程自己的任务体栈上 |
| `CPromiseState::SetTraceLink(upstream, mode, bChainRoot, nChainId)` | 每层记下「挂在哪一层之下」（**强引用**，注册时设一次、之后只读）、自己的模式、是不是链根、链号 —— 全写进一个 `m_trace`（`CLayerInfo`） |
| `VisitLayerChain` / `CurrentLayer` / `DescribeLayer` / `DescribeLayerChain` / `DescribeLayerChainBlock` / `DumpLayerChain` | 业务侧只读接口（`DescribeLayerChain` = 一行压缩链；`*Block` / `Dump` = 逐层富信息的多行排障块，深链自动头尾 + 省略） |

```cpp
// MakeLayerRunner 的任务体（唯一跑用户处理器的地方）—— 接入点就这一行（fnBody 由 MakeThenRunner / MakeResultRunner 给出）
return [pState, fnBody]()
{
#if defined(ASYNC_DEBUG_TRACE)
    const CCurrentLayerFrame frame(pState.get());
#endif
    ...
};
```

### 一层记什么（`CLayerInfo`：一份数据一个类型）

以前是「层状态里一份记录 + 对外一份视图」两个结构，字段大半重复；现在**合成一个**
`CLayerInfo`：上半部分是层自己的记录，下半部分是遍历时算出来的视图字段（层状态里不存这些）。
遍历时把记录原样带出来、只补视图字段 —— 不用逐字段搬运，也不用维护两份结构的同步。

| 分组 | 字段 | 说明 |
| --- | --- | --- |
| 记录（写一次，之后只读） | `loc` / `eMode` / `eKind` | 注册点（`ASYNC_LOC`）、模式（then / catch / finally）、本层类别（读 / 写 / 直投） |
| | `upstream` | 上游层（**强引用**，见下）；链根为空 |
| | `nLayerId` / `nChainId` | 全局递增的层号 / 链号（链号在链根分配，子链与父链不同号） |
| | `bChainRoot` / `bSubChain` | 是不是链根 / 是不是「挂在别的层下面」的子链链根 |
| | `tid` / `nSelfMs` / `tCreated` | 实际跑在哪条线程 / 本层处理器耗时 / 创建时刻 |
| | `spExecName` | 本层**实际跑在哪个执行器**上（执行器名）。`shared_ptr<const string>`：名字串在执行器构造时分配一次、各层共享，层记录持强引用 → 执行器析构后已起的链也不会悬垂 |
| 视图（遍历时算） | `nDepth` / `bCurrent` | 距当前层几跳 / 是不是正在执行的那一层 |
| | `nAgeMs` / `nSelfMs` | 年龄 = 创建到现在（链根上 = 整条链的年龄）；当前层的耗时用实时值 |
| | `bSettled` / `bFulfilled` / `strMessage` | 落定与否 / 结果是否兑现 / 拒绝文案（兑现为空） |

`DescribeLayer(info)` 把上面这些拼成一行（`examples` 里 ①…⑬ 每个位置打印的就是它）：

```text
#0   then    写  BuildOrderChain  main.cpp:641  [main]       链#1  层#3   龄=0ms 本层=0ms 结果=未落定 tid=…  ← 当前层
#1   then    读  BuildOrderChain  main.cpp:639  [main]       链#1  层#2   龄=0ms 本层=0ms 结果=兑现   tid=…
#2   then    写  BuildOrderChain  main.cpp:637  [main]       链#1  层#1   龄=0ms 本层=0ms 结果=兑现   tid=… [链根]
```

第三列（`%-3s`）是**本层类别**（`读` / `写` / `直` —— `直` = 直投、不过门），
方括号那一列（12 列，`[name]` 连方括号一起左对齐）是**本层实际跑在哪个执行器上**。
来源是**线程自己的归属**（`CThreadPool` 的 worker 在自己线程的 TLS 里带着池名，
`CCurrentLayerFrame` 压帧时顺手记下来），不是「注册时指定的执行器」：

- 链上的层 → **本链执行器**的名字（跨执行器就跨链：子链自己的执行器会让那两层的名字变掉）；
- `-`：本层没跑过 handler（桥接层、被跳过的层）或者跑在**非执行器线程**上
  （调用者线程等）—— 配合 `tid` 一起看（`tid=-` 才是「真没跑过」）；
- 名字只在执行器构造时给（`CAsyncExecutor("db", 4)`；空串 = 未命名 → `-`）。

「结果」是在**读的时候**从层状态里取的（`TryGetResult()`，锁内拷一份），所以正在跑的当前层
显示「未落定」、跑完的层显示兑现 / 拒绝（含异常描述）—— 失败路径上「被跳过的层照样在链上、
但结果停在上一层的拒绝」一眼就能看出来。

### 四个设计决定（都是为了「不改签名、不增加分配」）

1. **用 thread_local 而不是把「当前层」塞进处理器签名**：处理器的两种形状只为 then / catch 的需要而定，
   trace 不占形参（否则全框架的处理器都要为调试设施多带一个参数）。内联级联会**嵌套**跑层，所以帧是**栈语义**（RAII 保存 / 恢复），
   异常路径也不会漏弹（`Trace_FramePoppedAfterThrow` 守着）。
2. **上游直接存指针（仅调试构建），不用快照链表**：快照链要每层一次 `new`，而分配护栏（§12）
   在 debug 下跑 —— 会直接变成 3.03 次/层；直接存一个上游指针只有 16 B、**零分配**。
   中间试过 `weak_ptr`，**行不通**：层状态是靠「上游的处理器闭包」保活的，闭包用完即毁 →
   中间层一跑完就被释放，链被截断成两节（恰恰在最需要它的时候没用）；同理也不能
   「落了定就把上游放掉」。所以最终是**强引用 + 注册时设一次、之后只读**：
   只要下游还活着，上游就不会被释放 → 链总是完整的。
   代价（**仅调试构建**）：持有尾层句柄会把整条前缀留住；发布构建下这个字段根本不存在。
3. **当前层由帧保证存活，往上的每一跳先升强引用再访问**：否则
   「取到裸指针 → 上一跳的引用析构 → 节点被释放」就悬垂了。
4. **整个设施与 `ASYNC_LOC` 同一个开关**（调试构建 `ASYNC_DEBUG_TRACE`），并且**只在调试构建存在**：
   发布构建下 `CLayerInfo` 与所有函数整段在 `#if` 之外 —— **不是空操作版本**，连类型都拿不到。
   这样「调试专用」在编译期就是明确的，也不会留下一堆返回空值的假接口；
   代价是调用点要自己包 `#if defined(ASYNC_DEBUG_TRACE)`（示例里用 `TRACE_ONLY` 宏压成一行）。
   编辑器里这段代码发灰 = clangd 按 release 解析了，见 [vscode-clangd-format.md §7](../vscode-clangd-format.md)。

### 边界（写进文档，不是实现偷懒）

- **只能看「当前层 + 上游」**：`Then` / `ThenPromise` 是运行期随时可挂的（settle 之后也能挂），
  所以下游不存在「已知的形状」；
- 分叉 → 树；组合器（`WhenAll` 一族）→ 多父一子；
- **通知（`OnSettled`）不是层**：它不新建帧；就地送达时看到的是触发它的那一层；
- **子链 → 父链（已打通）**：起链时把「正在跑的那一层」记成新链链根的父层 —— 内层链（`ThenPromise`）、
  跨模块子链（`ThenBridge`）、组合器聚合链、层里 fire-and-forget 起的小链都能从子链里一路追回父链。
  两个细节：
  - 父层取的是「**真正在等这条子链的那一层**」（`Adopt` 里用 `detail::CChainAdopterScope` 显式指定）：
    工厂是在**上游层**的 settle 路径里跑的，不指定就会落回上游层，链上会看不到 `ThenPromise` 那一层；
  - 写入时机是**起链时、投递之前**（`StartChain` / `NewFromHandle` 里 `SetUpstream`）：
    链根一旦跑起来就可能被读，之后只读 —— 所以这条边**天生没有竞态**，不需要额外同步。
    例外：工厂返回的若是**别处早已建好的**链，它保留原来的归属（不重挂）；
    协程起的子链挂在「启动协程的那一层」（`CoStart` 处，与恢复时机无关，所以是确定的）。

### 测试

`Tests/test_async_trace.cpp` 的主用例就是**一条复杂主链**：① 具名 then（链根）→ ② 具名 then
（本链线程上就地级联）→ ③ 具名 then → ④ 被跳过的 `Catch` → ⑤ `Finally`
→ ⑥ `ThenPromise` 内层链 → ⑦ 分叉基座 → ⑧/⑨ 两支；然后在**最深处**把整条链逐层断言出来
（层数 / 模式 / 注册点行号 / 深度 / 「当前层」标记 / **跑在哪台执行器上** / 一行描述），
并逐个钉住特殊位置：

| 位置 | 断言到的结论 |
| --- | --- |
| 主链最深（分叉分支 B） | 8 层、深度 0…7，其中包含**被跳过的 `Catch` 层**；dump 块 = 头行 + 8 层（含 `← 当前层`）|
| 分支 A | 同一条主链前缀，但**看不到兄弟分支** |
| 跨执行器（子链跑在另一个执行器上） | 链照样完整；链上的层都在本链执行器上（父链的层从 `[trace-side]` 回到 `[trace-main]`），内层两层在同一条 worker 上且与父链线程不同 |
| 内层链（`ThenPromise`） | 链根挂在起它的那一层（`ThenPromise` 层）下面 → **一路追回主链**（8 层） |
| `OnSettled`（落定前登记） | 在**触发它的那一层**的帧里就地执行 |
| `OnSettled`（落定后登记） | 投递执行 → 不在任何层里（通知不是层） |
| 协程 `CO_AWAIT` | 子链挂在「启动协程的那一层」下面；恢复点是否在层里取决于就地 / 投递续跑，只断言这个上界 |
| 层内抛异常 | 抛之前链是完整的；抛之后帧栈干净 |
| 层外起的链 | 没有父层（链根就是链根，1 层） |

另外五个用例专蹭「复杂形状」（`test_async_trace.cpp` 共 6 例）：

| 用例 | 钉住的东西 |
| --- | --- |
| 多层子链（主链 → sub1 → sub2） | sub2 最深处能看到**跨 3 条链**的 7 层祖先路径；链号分成 3 段、段边界正好落在两条子链的链根上；子链链号 > 父链链号 |
| 多层子链（同上） | **「等子链」的那一层此刻还没落定**（`ThenPromise` 要等子链落定才落定）—— 所以子链内部能看到「谁还在等我」；其余已跑过的祖先都已落定且兑现 |
| 并发 4 条链（前缀层数 0/1/2/3 当指纹） | 每条链采到的层数与行号都等于**它自己**那条；4 条链的层号两两不同（没串链、上游没成环） |
| 深链（256 层）+ 帧栈残留 | 深度逐层对上、链号恒定、层号沿上游严格递减；跑完 / 抛异常两条路径都不在唯一 worker 上留帧（投个探针查）；上一条链的帧不影响下一条；dump 块默认头尾 + 「省略中间」（`0` = 不限，全量 257 行）|
| 层里 fire-and-forget 起链 | 父层 = **正在跑的那一层**；同时验证`CChainAdopterScope`**弹回**了（前面那次 `ThenPromise` 留下的作用域不能劫持后面的起链） |

这套用例用「注入故障」验过**有牙**：① 帧不弹栈 → 段错误；② 子链不挂父链 → 3 例失败；
③ `FillView` 深度 +1 → 5 例失败；④ 采纳作用域不弹栈 → 崩溃。

另一个用例只钉「层外是空操作」这条契约。发布构建下反过来断言「按契约全是空操作」。

> 写用例时的三个坑：
> 1. 采集层如果写成 then 式（只接上下文、一味返 `Resolve()`），
>    放到 catch 位置就什么也采不到（catch 层**一定**会看到拒绝）—— 采集层得用 catch / finally 签名；
> 2. 采集层应该**原样透传**上一层结果：在 catch 位置返回 `Resolve()` 会把拒绝吞掉（链被“恢复”），
>    用例对链走向的预期会跟着变；
> 3. **注册点行号要单行采集**：`nLine = __LINE__ + 1;` 之后挂层语句必须落在同一行 —— 多行实参
>    （`ASYNC_LOC` 被挤到第二行）记下的是**那一行**的行号，断言会差一行。内层链的工厂因此
>    单独具名（`PromiseFactory fnInnerChain = ...`），再 `ThenPromise(fnInnerChain, ASYNC_LOC)` 一行写完。
>
> 两个「让用例确定」的手法：**手动放行门**（`StepGated` 等主线程置位才返回）保证「通知是在
> 落定**之前**登记的」；**恢复点只断言上界**（就地续跑 → 落在被 await 的那层帧里；投递续跑 →
> 不在层里）—— 后者取决于线程池有没有积压，钉死会变脆。

## 附：代码阅读顺序

```text
1. Common/Async/PromiseResult.h     层结果（层间唯一信息）
2. Common/Async/PromiseTypes.h      两种处理器形状（ThenHandler / ResultHandler / SettledNotice）
3. Common/Async/AsyncExecutor.h     调度层与执行器句柄（含 detail::ShouldInline / DispatchInlineOrPost）
   Common/Async/Combine.h           组合器（WhenAll 一族：聚合状态 / 登记路径 / Gather）
   Common/Async/Diagnostics.{h,cpp} 诊断钩子（与执行器无关的进程级出口）
4. Common/Async/PromiseState.h     层状态机（单向开关 + 处理器登记 / 通知）
   Common/Async/PromiseLayer.h     层运行器（三态语义 + Make*Runner；含 ASYNC_GATE 重入）
   Common/Async/PromiseCore.h      共享核心（上下文 + 执行器句柄 + 派发）
5. Common/Async/Promise.h           句柄（重点看 AppendThenLayer / AppendResultLayer / RunThenHandler）与起链
   （层派发策略细节在 Common/Async/AsyncExecutor.h：detail::DispatchInlineOrPost / ShouldInline）
5. Common/Coroutine/Coroutine.h    顺序化（Duff's device 状态机）—— 另一个模块，只依赖 Async
6. Tests/test_async_chain.cpp       行为契约
7. Tests/test_async_alloc.cpp       分配预算护栏（改了热路径先看它）
```
