# 异步链 CAsyncChain — 实现文档

> 对应目录：`Common/Async`（命名空间 `common::async`）
> 使用方式见：[async-usage.md](async-usage.md) ｜ 协程见：[coroutine-impl.md](coroutine-impl.md)

## 1. 总体架构

```text
CAsyncExecutor                 调度层：CThreadPool + 执行器句柄（Start/Stop/Post）
    │  Handle()（shared_ptr<CExecutorHandle>）
    ├── CAsyncChain<TContext>  编排层：链段（每层一段）+ 共享上下文
    │        │
    │        └── CStepResult   层结果（层间唯一传递的信息）
    └── CCoroutine<TContext>   顺序层：用顺序代码 await 多条链（见 coroutine-impl）
```

文件划分（`Common/Async/`）：

| 文件 | 内容 |
| --- | --- |
| `StepResult.h` | `CStepResult`（层结果：成功 / 失败 + 错误码）、`StepCode` 常量 |
| `AsyncTypes.h` | `CCompletedFn`、`detail::StepFn<TContext>`（层函数固定签名） |
| `SourceLoc.h` | `CSourceLoc` + `ASYNC_LOC`（注册点调试信息，发布构建零开销） |
| `AsyncExecutor.h/.cpp` | `CAsyncExecutor`、`detail::CExecutorHandle`、`detail::PostToHandle` |
| `AsyncChain.h` | `detail::CChainSegment`、`detail::CChainCore<TContext>`、`CAsyncChain<TContext>` |
| `Coroutine.h` | `CCoroutine<TContext>` + `CO_*` 宏 |

## 2. 为什么固定签名 + 共享上下文

传值版任务链（`CTask<TValue>`）要求框架为每层保存一个 `CTaskResult<TValue>`，
并做类型萃取 / 分派（`TaskTraits` / `RunTransform` 一整套）——链的类型随层数变化，
编译期开销与实现复杂度都不小。

本版把「值」移出层间通道：

```text
层间只传：CStepResult（int 错误码，双向零分配、可平凡拷贝）
数据通道：shared_ptr<TContext>（一次流程一个实例，整条链共用）
```

收益：

- 链只有一种类型 `CAsyncChain<TContext>`，`Then` 返回同类型 → 无需类型萃取与分派；
- 层的签名统一 → 自由函数 / 静态成员 / `bind` / lambda 都能直接注册；
- 跨层数据不再需要拷贝（上下文是引用语义），大对象（报文、连接、DB 句柄）不再层层复制；
- 失败与成功都是同一枚 `CStepResult`，语义简单可预测。

代价：

- 层与层之间没有「类型安全的显式数据通道」，数据约定靠 `TContext` 的字段约束；
- 层之间不再能传递一次性临时值（必须落到上下文里）。

## 3. CExecutorHandle：生命周期加固

```cpp
struct CExecutorHandle
{
    std::shared_ptr<common::thread::CThreadPool> m_pPool; // 线程池（共享持有）
    std::atomic<bool> m_bStopped;                         // 是否已停止（拒绝新投递）
};
```

- `CAsyncExecutor` 持有该句柄；链 / 协程各自持一份 `shared_ptr`；
- 执行器析构 → `Stop()` → 置 `m_bStopped` 并 `pool->Stop()`（等待已投递任务完成）；
  由于句柄仍被链持有，线程池对象**不会悬垂**，已起的链照常跑完；
- 之后的新投递被 `m_bStopped` 拒绝 → 对应层以 `kStepStopped` 失败。

`detail::PostToHandle(handle, fn)` 是唯一投递入口：句柄为空 / 已停止 / 池拒绝都返回 `false`，
调用方据此把结果置为 `kStepStopped`（不抛异常）。

## 4. CChainSegment：一段的状态机

一道链由若干**链段**串成，一层对应一段。段是单向开关：

```cpp
class CChainSegment
{
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<Continuation> m_vecContinuations; // 未完成时登记
    std::atomic<bool> m_bReady;                   // 自旋读 + 等待谓词
    CStepResult m_result;                         // 完成后有效
    CSourceLoc m_loc;                             // 注册点（仅调试构建）
};
```

| 成员 | 语义 |
| --- | --- |
| `Complete(result)` | 首次生效：锁内置结果与 `m_bReady`，换出续接列表，`notify_all` 后在**锁外**按序调用续接 |
| `AddContinuation(handle, cb)` | 未完成 → 登记返回 true；已完成 → 投递 `cb` 到执行器异步触发；已完成且执行器不可用 → 返回 false |
| `Wait()` | 先自旋 50μs（宽松提示），再在条件变量上阻塞；`notify_all` 支持多线程等待同一段 |
| `SetLoc/Loc` | 注册点源码位置（发布构建空实现） |

两个关键设计：

1. **续接在锁外调用**：层函数可能在续接里执行（级联），若持锁调用会因重入造成死锁；
2. **已完成再注册走投递**：与 JS / C# 的 promise 一致 —— 注册方（可能是业务线程）
   不会被回调阻塞，回调在工作线程上跑。

## 5. 一次链的完整生命周期

以 `exec.Submit(spCtx, f0).Then(f1).Then(f2)` 为例：

```text
① Submit：建核心（spCtx + 句柄）+ 建段 s0 → PostStep（投递 f0，起点结果 Ok）
                       ↓（工作线程 W）
② f0(Ok, spCtx) 执行 → s0.Complete(r0)
       ├─ 已登记续接（f1 那段 s1）：r0 失败？→ s1.Complete(r0)（短路）；否则 RunStep 执行 f1
       └─ 已登记的完成回调（若有）
                       ↓（仍是线程 W，级联）
③ f1(r0, spCtx) → s1.Complete(r1) → 同路径推进 f2
                       ↓
④ f2 → s2.Complete(r2) → Get() 被唤醒 / OnCompleted 触发
```

要点：

- **首层必须投递**（`PostStep`）：起链线程不执行任何业务代码；
- **后续层在同一线程级联**（`RunStep` 内联）：一层链只花一次入队 + 唤醒，
  而不是每层一次；
- **失败短路不调用层函数**：`Then` 的续接里 `upStep.IsFailed()` 直接
  `s_next.Complete(upStep)`，层函数不被调用（层函数里的 `if (upStep.IsFailed()) return upStep;`
  是防御性写法）；
- **`ThenAlways` 走同一续接，但跳过失败短路分支**，因此层函数会以失败状态的
  `upStep` 被调用。

### 级联内联与深度限制

```cpp
std::function<void()> fnRun = MakeStepRunner(...);   // 执行层函数 + 完成本段
if (InlineDepth() < kMaxInlineDepth)   // 线程局部深度计数（64）
{
    ++InlineDepth(); fnRun(); --InlineDepth();       // 直接执行（省一次投递 + 唤醒）
}
else
{
    PostToHandle(pCore->Handle(), std::move(fnRun)); // 超限改投递，防递归爆栈
}
```

`InlineDepth()` 是线程局部计数器，链的级联与协程的内联续接**共用**它，
因此「链 + 协程」混合递归也被同一上限保护。

## 6. 两种注册时机

| 注册时刻 | 路径 | 执行线程 |
| --- | --- | --- |
| 上游**未完成** | 登记到 `m_vecContinuations`，上游 `Complete` 时被调用 | 上游完成所在的工作线程（级联内联） |
| 上游**已完成** | `AddContinuation` 检测到 `m_bReady` → 投递回调 | 执行器工作线程 |

两者都不阻塞调用方，且都保证「本层只执行一次」。差别只在执行时机与落点线程 ——
业务侧无需关心（框架把顺序保证放在链段上，而不是调用线程上）。

## 7. 失败语义的实现

```cpp
[pCore, pNextSegment, fnStep, bAlways](const CStepResult& upStep)
{
    if (upStep.IsFailed() && !bAlways)      // Then：失败即停
    {
        pNextSegment->Complete(upStep);     // 不执行层函数，原样透传失败码
        return;
    }
    detail::RunStep(pCore, pNextSegment, fnStep, upStep); // upStep 可能携带失败（ThenAlways）
}
```

- 短路只做一次 `Complete`，代价极小 → 失败链的层数越多，反而越省（基准中
  `CAsyncChain fail-fast x20` 与成功链同量级）；
- 失败码全程**原样透传**，框架不改写业务码；框架自身错误码仅 4 个（见 `StepCode`）；
- `RunStep` 内 `try/catch(...)` 把层内异常转 `kStepException`，保证 `Get()` 不抛。

## 8. 线程模型与不变量

| 不变量 | 保证方式 |
| --- | --- |
| 一条链的层不并发 | 段的续接只在 `Complete` 时按序触发一次，级联在同一线程推进 |
| 同一段只完成一次 | `Complete` 锁内 `m_bReady` 判定（CAS 语义），后续调用直接返回 |
| 层函数不在起链线程执行 | `Submit` 固定走 `PostStep` |
| 回调不持锁 | `Complete` 先换出续接列表，再锁外调用 |
| 无悬垂 | 句柄 / 段 / 上下文均为 `shared_ptr`，被续接与句柄共同持有 |
| 深链不爆栈 | `kMaxInlineDepth` 上限 + 改投递 |

分叉（同一段注册多个 `Then`）时，各支线是独立段：它们由上游 `Complete` 依次触发，
若走的是「已完成再注册」路径则各自投递 → 可能在**不同线程并行**，各自持有同一上下文，
业务需自行保证上下文字段访问安全。

## 9. 源码位置调试（ASYNC_LOC）

```cpp
#if defined(__linux__) && !defined(__OPTIMIZE__)
    #define ASYNC_DEBUG_TRACE 1
#endif
#define ASYNC_LOC common::async::CSourceLoc(__PRETTY_FUNCTION__, __FILE__, __LINE__)
```

- 调试构建（`-O0`）：每段保存注册点的函数名 / 文件 / 行号 → 调试器 watch 段对象的
  `m_loc` 即可定位「这一层是谁注册的」；
- 发布构建（`-O2`）：`CSourceLoc` 为空、**不保存**，`SetLoc/Loc` 退化为空操作（零开销）；
- 固定签名层的注册点往往是一串 lambda，注册点信息是定位「哪一层失败」的最直接手段。

## 10. 设计取舍

1. **eager（起链即投递）**：`Submit` 立即投递首层，不做惰性计划 —— 语义简单，
   与 `Post` 一致；代价是无法在起链前再改链结构（要改就多注册一层）。
2. **句柄指向某一层**（而非整条链）：`Then` 返回新句柄，`Get/OnCompleted` 作用于
   句柄所指的段。这样天然支持分叉、支持「链跑完后追加层」，也不必维护「链尾」指针
   （分叉时链尾不唯一）。
3. **失败即停 + ThenAlways**：默认安全（忘写判断也不会误执行后续业务），
   同时给回滚 / 补偿留了显式出口（`ThenAlways` 里 `return upStep;` 即传统透传，
   `return Ok();` 即吞掉失败恢复链）。
4. **不做重复完成 / 取消**：段是单向开关，没有取消 API。需要超时或取消时，
   在业务层用 `IEventDispatcher` / 定时器唤醒后检查标志位。
5. **上下文不进层签名之外**：不提供「向上下文追加任意类型」的容器（如 `std::any`），
   以保证 `TContext` 的字段在编译期可查、无堆分配、无类型擦除开销。

## 11. 测试与基准

- 单元测试：`Tests/test_async_chain.cpp`（31 个用例：契约、顺序、上下文、失败即停、
  `ThenAlways` 观察 / 恢复、异常、完成回调、分叉、完成后追加、未启动 / 停止 / 重启、
  工作线程、析构后完成、并发 Get、多链并行、深链 300 层、400 链压力、协程 9 例）；
- 基准：`Benchmark/cases/ChainCase.cpp`（层数 1/5/20/100、深链 256、失败即停）、
  `CoroutineCase.cpp`、`ResumableCase.cpp`、`StressCase.cpp`；
- 示例：`examples/main.cpp`；业务侧用法见 `ServerExample/Module/ExampleAsyncModule.cpp`。

## 附：代码阅读顺序

```text
1. Common/Async/StepResult.h        层结果（层间唯一信息）
2. Common/Async/AsyncTypes.h        固定签名（StepFn）
3. Common/Async/AsyncExecutor.h     调度层与执行器句柄
4. Common/Async/AsyncChain.h        链段 + 核心 + 链（重点看 Complete / AddContinuation / RunStep）
5. Common/Async/Coroutine.h         顺序化（Duff's device 状态机）
6. Tests/test_async_chain.cpp       行为契约
```
