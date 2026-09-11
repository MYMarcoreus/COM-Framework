# 异步 promise CPromise — 实现文档

> 对应目录：`Common/Async`（命名空间 `common::async`）
> 使用方式见：[async-usage.md](async-usage.md) ｜ 协程见：[coroutine-impl.md](coroutine-impl.md)

## 1. 总体架构

```text
CAsyncExecutor                 调度层：CThreadPool + 执行器句柄（Start/Stop/Post/NewPromise）
    │  Handle()（shared_ptr<CExecutorHandle>）
    ├── CPromise<TContext>     编排层：承诺状态（每层一个）+ 共享上下文
    │        │
    │        └── CPromiseResult  层结果（层间唯一传递的信息：兑现 / 拒绝）
    └── CCoroutine<TContext>   顺序层：用顺序代码 await 多条 promise（见 coroutine-impl）
```

文件划分（`Common/Async/`）：

| 文件 | 内容 |
| --- | --- |
| `PromiseResult.h` | `CPromiseResult`（兑现 / 拒绝 + 错误码）、`PromiseCode` 常量 |
| `PromiseTypes.h` | `SettledHandler`、`detail::ThenHandler<TContext>`（处理器固定签名） |
| `SourceLoc.h` | `CSourceLoc` + `ASYNC_LOC`（注册点调试信息，发布构建零开销） |
| `AsyncExecutor.h/.cpp` | `CAsyncExecutor`、`detail::CExecutorHandle`、`detail::PostToHandle` |
| `Promise.h` | `detail::CPromiseState`、`detail::CPromiseCore<TContext>`、`CPromise<TContext>` |
| `Coroutine.h` | `CCoroutine<TContext>` + `CO_*` 宏 |

## 2. 为什么固定签名 + 共享上下文

传值版任务链要求框架为每层保存一个值类型结果，并做类型萃取 / 分派（`TaskTraits` /
`RunTransform` 一整套）——链的类型随层数变化，编译期开销与实现复杂度都不小。

本版把「值」移出层间通道：

```text
层间只传：CPromiseResult（int 错误码，零分配、可平凡拷贝）
数据通道：shared_ptr<TContext>（一次流程一个实例，整条链共用）
```

收益：

- promise 只有一种类型 `CPromise<TContext>`，`Then` / `Catch` / `Finally` 返回同类型 → 无需类型萃取；
- 处理器签名统一 → 自由函数 / 静态成员 / `bind` / lambda 都能直接注册；
- 跨层数据是引用语义，大对象（报文、连接、DB 句柄）不再层层拷贝；
- 兑现与拒绝是同一枚 `CPromiseResult`，配合 JS 命名的 then / catch / finally，语义直白。

代价：层间没有「类型安全的显式数据通道」，数据约定靠 `TContext` 的字段约束。

## 3. CExecutorHandle：生命周期加固

```cpp
struct CExecutorHandle
{
    std::shared_ptr<common::thread::CThreadPool> m_pPool; // 线程池（共享持有）
    std::atomic<bool> m_bStopped;                         // 是否已停止（拒绝新投递）
};
```

- `CAsyncExecutor` 持有该句柄；promise / 协程各自持一份 `shared_ptr`；
- 执行器析构 → `Stop()` → 置 `m_bStopped` 并 `pool->Stop()`（等待已投递任务完成）；
  句柄仍被 promise 持有 → 线程池对象**不会悬垂**，已起的 promise 照常跑完；
- 之后的新投递被 `m_bStopped` 拒绝 → 对应层以 `kStopped` 被拒绝。

`detail::PostToHandle(handle, fn)` 是唯一投递入口：句柄空 / 已停止 / 池拒绝都返回 `false`，
调用方据此把结果置为 `Reject(kStopped)`（不抛异常）。

## 4. CPromiseState：一层的状态机

一条 promise 链由若干**状态**串成，一层一个（对应 JS 中「每个 then 返回的新 promise」）：

```cpp
class CPromiseState
{
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<Handler> m_vecHandlers;  // pending 时登记
    std::atomic<bool> m_bSettled;        // 自旋读 + 等待谓词
    CPromiseResult m_result;             // settled 后有效
    CSourceLoc m_loc;                    // 注册点（仅调试构建）
};
```

| 成员 | 语义 |
| --- | --- |
| `Settle(result)` | 首次生效：锁内置结果与 `m_bSettled`，换出处理器列表，`notify_all` 后在**锁外**按序调用 |
| `AddHandler(handle, cb)` | pending → 登记返回 true；已 settled → 投递 `cb` 到执行器异步触发；已 settled 且执行器不可用 → false |
| `Await()` | 先自旋 50μs，再在条件变量上阻塞；`notify_all` 支持多线程等待同一 promise |
| `SetLoc/Loc` | 注册点源码位置（发布构建空实现） |

两个关键设计：

1. **处理器在锁外调用**：处理器内部可能触发下一层的级联，持锁调用会因重入造成死锁；
2. **已 settled 再注册走投递**：与 JS 一致 —— 注册方（可能是业务线程）不被回调阻塞。

## 5. 一次 promise 链的完整生命周期

以 `exec.NewPromise(spCtx, f0).Then(f1).Then(f2)` 为例：

```text
① NewPromise：建核心（spCtx + 句柄）+ 建状态 s0 → PostHandler（投递 f0，起点结果 Resolve）
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

- **首层必须投递**（`PostHandler`）：起 promise 的线程不执行任何业务代码；
- **后续层级联**（`RunHandler`）：若当前线程已是本链执行器的线程 → 就地内联（省一次入队）；
  **否则（跨执行器，比如被调模块 settle 本链）投递回本链执行器** —— 见「线程亲和」；
- **then 失败即停不调用处理器**：续接里 `upResult.IsRejected()` 直接 `Settle(upResult)`；
- **catch / finally 走同一续接，但分派不同**（见下节）。

### 级联内联与深度限制（含线程亲和）

```cpp
auto fnRun = MakeHandlerRunner(...);                       // 执行处理器 + settle 本层
if (IsInExecutorThread(pCore->Handle())                    // ① 线程亲和：必须在本链执行器线程上
    && InlineDepth() < kMaxInlineDepth)                    // ② 深度未超限（线程局部计数 64）
{
    ++InlineDepth(); fnRun(); --InlineDepth();              // 就地执行（省一次投递 + 唤醒）
}
else
{
    PostToHandle(pCore->Handle(), std::move(fnRun));        // 跨执行器 / 深度超限 → 投递回本链执行器
}
```

`InlineDepth()` 是线程局部计数器，链的级联与协程的内联续接**共用**它，
因此「promise + 协程」混合递归也被同一上限保护；加上①后，深度只在**同一执行器线程内**累加，
跨模块不会涨栈。

### 5.1 两种启动模式：立即启动 vs 延迟启动（`BuildPromise`，改进 C）

| 模式 | 入口 | 追加层 | 首层何时投递 |
| --- | --- | --- | --- |
| 立即启动（默认） | `exec.NewPromise(spCtx, 首层)` / `CPromise(exec, spCtx, 首层)` | 链已在跑，追加可能落在“已 settled”路径上 | 调用即投递 |
| 延迟启动 | `exec.BuildPromise(spCtx)` | 只登记（首层动作暂存在 `CLaunchState::fnLaunch`） | `Start()`（或首次 `Await()`）才投递 |

```cpp
// Common/Async/Promise.h
extern struct CLaunchState { bool bDeferred; bool bStarted; std::function<void()> fnLaunch;
                            std::shared_ptr<CPromiseState> pFirst; std::shared_ptr<CExecutorHandle> pTarget; };
// Append / ThenPromise / New 在 bDeferred 时只登记启动动作；Start() 投递它（幂等）；
// Await() 发现“延迟链未启动”则自动 Start()（兜底）。
```

要点：

- 延迟链**构链期不跑任何业务代码**（连 `New(...)` 的 executor 都延后到轮到该层才执行）；
- 所有层都在首层开跑前登记完毕 → 跨模块续接不再出现“补登”的时序差异（对第 7 节的两种注册时机是个限定）；
- 首层启动仍尊重 `kAffinityExecutor`（`pTarget`），启动失败（执行器已停）以 `kStopped` 收口首层；
- `Await()` 对未启动的延迟链自动 `Start()`，所以漏写 `Start()` 不会死等；
- 普通链行为完全不变（`bDeferred == false`）。

## 6. 处理器模式分派（then / catch / finally）

`Append(handler, loc, nMode)` 是 `Then` / `Catch` / `Finally` 的共同实现，差异只在续接分派：

```cpp
[pCore, pNextState, fnHandler, nMode](const CPromiseResult& upResult)
{
    // then：上一层被拒绝 → 失败即停（本层不执行，拒绝原因原样交给下一层）
    if (nMode == detail::kModeThen && upResult.IsRejected())
    { pNextState->Settle(upResult); return; }

    // catch：上一层已兑现 → 无事可做，原样交给下一层
    if (nMode == detail::kModeCatch && upResult.IsFulfilled())
    { pNextState->Settle(upResult); return; }

    // finally：无论成败都执行（但忽略返回值）；then / catch：执行本层处理器
    detail::RunHandler(pCore, pNextState, fnHandler, upResult, nMode);
}
```

处理器执行体（`MakeHandlerRunner`）里对 `finally` 做了收尾处理：

```cpp
const CPromiseResult own = fnHandler(upResult, pCore->Context());
result = (nMode == kModeFinally) ? upResult : own;   // finally 不改变结果
```

- `then` / `catch`：返回值即本层结果 → 决定后续走向（catch 返回 `Resolve()` 即恢复）；
- `finally`：返回值被忽略，原样透传 `upResult`；只有抛异常才会改变结果（→ `Reject(kException)`），
  与 JS `finally` 语义一致。

**首层特例**：尚未起链时第一次 `Then` / `Catch` / `Finally` 即首层，起点结果视为「已兑现」。
因此 `Catch` 作为首层不会执行（没有可处理的拒绝），直接以 `Resolve()` settle。

### 6.1 跨模块组合的两个原语（`ThenPromise` / `CPromise::New`）

| API | JS 对照 | 实现要点 |
| --- | --- | --- |
| `CPromise<T>::New(exec, spCtx, executor, loc)` | `new Promise((resolve, reject) => …)` | 直接建 `CPromiseState` 并交出 `ResolveFn` / `RejectFn`（内部就是 `pState->Settle(...)`）；executor 同步执行（与 JS 一致），抛异常 → `Reject(kException)`；`Settle` 幂等，故重复 settle / settle 后异常都安全 |
| `CPromise<T>::ThenPromise(factory, loc)` | `then(处理器返回 promise)` 的 flatten | 建本层 state，在上游 state 上登记 handler：上游被拒 → 直接透传；上游兑现 → `Adopt()` |

`Adopt()` 做的事：调 `factory(spCtx)` 拿到子 promise，在**子 promise** 的 `OnSettled` 回调里
`pState->Settle(childResult)` —— 本层的 settle 由子 promise 的结果决定。注意点：

- **不阻塞**：全程只登记回调，不 `Await()`、不占工作线程（单线程执行器也安全）；
- 子 promise 的 settle 线程可能是**另一个模块的执行器线程** → 流程函数请按值捕获依赖与上下文，
  不要捕获本模块 `this`（这样流程是纯函数，任何线程上都安全）；
- 工厂抛异常 → 本层 `Reject(kException)`；工厂返回无效 promise → 本层 `Reject(kStopped)`；
  子 promise 的拒绝码**原样**成为本层拒绝码（后续 `Then` 不执行，`Catch` / `Finally` 仍执行）；
- **保活**：子 promise 的最后一段由「上一段 handler 捕获下一段」链保活，本层 state 被子 promise
  的 `OnSettled` handler 捕获 —— 即使句柄被丢弃，在途的整条链仍安全跑完。

## 7. 两种注册时机

| 注册时刻 | 路径 | 执行线程 |
| --- | --- | --- |
| 上一层**未 settled** | 登记到 `m_vecHandlers`，settle 时被调用 | 由「线程亲和」决定：在本链执行器线程上就地级联，否则投递回本链执行器 |
| 上一层**已 settled** | `AddHandler` 检测到 `m_bSettled` → 投递回调 | 本链执行器的工作线程 |

两者都不阻塞调用方，且都保证「本层只执行一次」；自 2026-09-11 的线程亲和（改进 A）起，
**两者的落点线程一致：都是本链执行器线程**（差别只剩执行时机：立即 vs 入队）。

`OnSettled` 走的是另一条路径（`AddSettledHandler`）：它是「通知」不是「层」，因此带**送达保证** ——
执行器可用时投递（同上表第二种），执行器不可用时（被调模块已停 / 拒绝投递）**在调用线程上就地执行**，
绝不丢弃（否则手写桥接漏检返回值就会让本层永久 pending、上层 `Await()` 死等）。
层处理器仍保持 `AddHandler` 的语义：执行器不可用 → 返回 `false` → 框架以 `kStopped` 收口本层。

## 8. 线程模型与不变量

| 不变量 | 保证方式 |
| --- | --- |
| 一条链的层不并发 | 状态只在 `Settle` 时按序触发一次处理器，级联在同一线程推进 |
| 同一状态只 settle 一次 | `Settle` 锁内 `m_bSettled` 判定，后续调用直接返回 |
| 处理器不在起链线程执行 | 首层固定走 `PostHandler` |
| **每层都在本链执行器线程上** | 线程亲和：`RunHandler` 先判 `IsInExecutorThread`，不满足就投递回本链执行器 |
| **跨模块返回的层回本模块** | 同上（被调模块 settle 本链时，本链层不在被调模块线程跑） |
| 回调不持锁 | `Settle` 先换出处理器列表，再锁外调用 |
| 无悬垂 | 句柄 / 状态 / 上下文均为 `shared_ptr`，被续接与句柄共同持有 |
| 深链不爆栈 | `kMaxInlineDepth` 上限 + 改投递（且只在同一执行器线程内累加） |

### 8.1 线程亲和（改进 A，2026-09-11）+ 逐层覆盖（改进 B）

亲和三档（`detail::HandlerAffinity`）：

| 取值 | 本层在哪跑 | 对外 API |
| --- | --- | --- |
| `kAffinityChain`（默认） | 本链执行器线程（已在该线程 → 就地内联；否则投递回本链执行器） | `Then` / `Catch` / `Finally` / `ThenPromise` |
| `kAffinityInline` | **结算本层的那条线程**上就地执行（不投递） | `ThenInline` |
| `kAffinityExecutor` | 指定执行器线程（已在该线程 → 就地；否则投递到它） | `ThenOn(exec, …)` |

```cpp
// Common/Thread/ThreadPool：worker 线程打 thread_local 标记
extern/static thread_local const CThreadPool* tl_pCurrentPool;   // WorkerLoop 进入设、退出清
static bool CThreadPool::IsInPoolThread(const CThreadPool* pPool);

// Common/Async/AsyncExecutor.h（detail）
inline bool IsInExecutorThread(const std::shared_ptr<CExecutorHandle>& pHandle)
{
    return pHandle != nullptr && CThreadPool::IsInPoolThread(pHandle->m_pPool.get());
}

// Common/Async/Promise.h：层处理器（detail::RunHandler）
const std::shared_ptr<CExecutorHandle> pExec =
    (nAffinity == kAffinityExecutor && pTarget != nullptr) ? pTarget : pCore->Handle();   // 选执行器
const bool bInline = (nAffinity == kAffinityInline) || IsInExecutorThread(pExec);         // 就地？
if (bInline && InlineDepth() < kMaxInlineDepth) { ++InlineDepth(); fnRun(); --InlineDepth(); }
else if (!PostToHandle(pExec, std::move(fnRun))) { pState->Settle(Reject(kStopped)); }       // 投递目标执行器
```

- `Append(fnHandler, loc, nMode, nAffinity, pTarget)`：亲和与目标句柄随注册的处理器一起捕获，
  并在“已 settled → 投递”路径上也用同一个目标执行器（`AddHandler(pExec, …)`）；
- **首层例外**：always 投递（起链线程不跑业务代码），`kAffinityExecutor` 时投递到目标执行器；
- **保证**：默认配置下每一层与协程的每一次续跑都跑在「它所属链的执行器线程」上；
- **代价**：每次跨执行器的续接多一次入队 + 唤醒（微秒级）；同执行器内仍完全内联；
  内联深度只在同一执行器线程内累加，跨模块不涨栈；
- **边界**：亲和只作用于「层」——`OnSettled` 通知仍在**结算线程**上触发（不可用时就地送达）；
  `New(...)` 的 executor 是「发起」语义，仍在调用线程上同步执行；`Await()` 仍占住调用线程；
- **验收**：`Tests/test_async_affinity.cpp`（5 例，默认亲和）+ `Tests/test_async_affinity_override.cpp`
  （4 例，`ThenInline` / `ThenOn` / 已停执行器 / 默认对照）+ `Tests/test_async_modules*.cpp`
  （当初发现问题的极限用例，现断言 200 条并发链 100% 落回本模块线程）。

分叉（同一状态注册多个 `Then`）时各支线是独立状态：由上游 `Settle` 依次触发，若走
「已 settled 再注册」路径则各自投递 → 各支线依次在同一执行器线程上执行，
但仍共享同一上下文，业务需自行保证上下文字段访问安全。

## 9. 源码位置调试（ASYNC_LOC）

```cpp
#if defined(__linux__) && !defined(__OPTIMIZE__)
    #define ASYNC_DEBUG_TRACE 1
#endif
#define ASYNC_LOC common::async::CSourceLoc(__PRETTY_FUNCTION__, __FILE__, __LINE__)
```

- 调试构建（`-O0`）：每个状态保存注册点函数名 / 文件 / 行号 → 调试器 watch 状态对象的
  `m_loc` 即可定位「这一层是谁注册的」；
- 发布构建（`-O2`）：`CSourceLoc` 为空、**不保存**，`SetLoc/Loc` 退化为空操作（零开销）；
- 处理器常是一串 lambda，注册点信息是定位「哪一层被拒绝」的最直接手段。

## 10. 设计取舍

1. **eager（起链即投递）**：`NewPromise` / 带处理器的构造函数立即投递首层，
   与 `new Promise(executor)` 立即执行 executor 一致；代价是无法在起链前再改结构。
2. **句柄指向某一层**（而非整条链）：`Then` 返回新句柄，`Await/OnSettled` 作用于句柄所指的状态。
   天然支持分叉、支持「settled 后追加层」，也不必维护「链尾」指针（分叉时链尾不唯一）。
3. **then / catch / finally 三分**：默认安全（`then` 忘写判断也不会在拒绝后误执行后续业务），
   同时给回滚（`Catch`）与收尾（`Finally`）留出与 JS 完全对应的显式出口。
4. **不做取消 / 超时**：状态是单向开关，没有取消 API。需要超时或取消时，
   在业务层用定时器 / 事件唤醒后检查标志位。
5. **上下文为编译期类型**：不提供「向上下文追加任意类型」的容器（如 `std::any`），
   以保证 `TContext` 字段编译期可查、无堆分配、无类型擦除开销。

## 11. 测试与基准

- 单元测试：`Tests/test_async_chain.cpp`
  - promise 22 例：签名契约、顺序与上下文、then 失败即停、catch 观察 / 恢复、finally 不改结果、
    then 与 catch 互补、构造即起链、异常、settled 通知、分叉、settled 后追加、未启动 / 停止 /
    重启、工作线程、析构后完成、并发 Await、多链条并行、深链 300 层、400 条压力、Post 行为；
  - 协程 9 例（见 coroutine-impl.md）；
- 基准：`Benchmark/cases/ChainCase.cpp`（层数 1/5/20/100、深链 256、失败即停）、
  `CoroutineCase.cpp`、`ResumableCase.cpp`、`StressCase.cpp`；
- 示例：`examples/main.cpp`；业务侧用法见 `ServerExample/Module/ExampleAsyncModule.cpp`。

## 附：代码阅读顺序

```text
1. Common/Async/PromiseResult.h     层结果（层间唯一信息）
2. Common/Async/PromiseTypes.h      固定签名（ThenHandler / SettledHandler）
3. Common/Async/AsyncExecutor.h     调度层与执行器句柄
4. Common/Async/Promise.h           状态 + 核心 + promise（重点看 Append / Settle / RunHandler）
5. Common/Async/Coroutine.h         顺序化（Duff's device 状态机）
6. Tests/test_async_chain.cpp       行为契约
```
