# 无栈协程 CCoroutine — 实现文档

> 对应文件：`Common/Async/Coroutine.h`
> 使用方式见：[coroutine-usage.md](coroutine-usage.md) ｜ 链实现见：[async-impl.md](async-impl.md)

## 1. 原理：Duff's device 状态机

无栈协程把「函数体」编译成一个带恢复点的状态机：每个 `CO_AWAIT` 处保存恢复点
（`__LINE__`）并 `return` 让出线程，任务完成后从该 `case` 继续执行。

```cpp
#define CO_BEGIN()  switch (Step()) { case 0:;

#define CO_AWAIT(expr) \
    AwaitWait(__LINE__, (expr)); \                 // 注册完成回调（挂起）
    return; \                                      // 让出线程
    case __LINE__: \                               // ← 恢复点
    if (IsTerminated()) { CompleteTerminated(); return; }
```

要点：

- `switch (Step())` 让状态机回到上次的恢复点；
- `case 0` 是首次进入（`CO_BEGIN`）；
- 每个 `CO_AWAIT` 独占一行，因为 `__LINE__` 就是它的标签；
- 恢复时若已终止（等待的链失败），统一走 `CompleteTerminated()`。

## 2. 内部结构

```cpp
template <typename TContext>
class CCoroutine
{
    std::shared_ptr<detail::CChainCore<TContext> > m_pCore;  // 执行器句柄 + 共享上下文
    std::shared_ptr<detail::CChainSegment> m_pSegment;       // 协程完成状态（AsChain 暴露）
    CAsyncExecutor* m_pExec;                                 // 调度（Resume + 子链投递）
    std::weak_ptr<void> m_wpSelf;                            // 自持弱引用（生命周期加固）
    CHotState m_hot;                                         // 步号 / 终止标志 / 终止码
};
```

`CHotState` 把三个热字段打包相邻，减少跨线程迁移时的 cache line 数：

```cpp
struct CHotState
{
    std::atomic<int> nStep;         // 状态机步号（恢复点）
    std::atomic<bool> bTerminated;  // await 到失败 → 终止
    std::atomic<int> nCode;         // 终止失败码
};
```

协程的**完成状态直接复用链段**（`detail::CChainSegment`）—— 这样 `AsChain()`
只要把 `(m_pCore, m_pSegment)` 包成链句柄，协程就自然成为一个可 await 的对象，
不需要第二套「完成通知」实现。

## 3. 生命周期加固（m_wpSelf）

所有会「稍后回来」的地方都先 `m_wpSelf.lock()` 取强引用，再捕获进回调：

```cpp
std::shared_ptr<void> spSelf = m_wpSelf.lock();
chain.OnCompleted([spSelf, this](CStepResult r) { ... });   // 回调期间对象保活
m_pExec->Post([spSelf, this]() { Resume(); });
```

因此调用方即使提前释放 `CoStart` 返回的 `shared_ptr`，协程对象也会存活到
最后一个 Resume / await 回调执行完毕（不悬垂）。

## 4. 启动流程

```text
exec.CoStart<TCoroutine>(args...)
    ├── make_shared<TCoroutine>(args...)     创建（构造时建 m_pCore 与初始 m_pSegment）
    ├── pCoro->SetSelf(pCoro)                注入自持弱引用
    └── pCoro->Start(this)
             ├── BindExecutor(pExec)         m_pExec = pExec；把执行器句柄写入 m_pCore
             ├── Reset()                    新建 m_pSegment；步号 / 终止标志复位
             └── PostResume()               投递首次 Resume（执行器不可用 → 立即以 kStepStopped 完成）
```

`Reset()` 让同一协程对象可以重新 `Start`（重复使用）。

## 5. 顺序 await（AwaitWait）

```cpp
void AwaitWait(int nLine, const CAsyncChain<TContext>& chain)
{
    m_hot.nStep.store(nLine);                          // 记恢复点
    std::shared_ptr<void> spSelf = m_wpSelf.lock();
    bool bOk = chain.OnCompleted([spSelf, this](CStepResult r)
    {
        if (r.IsFailed()) { MarkTerminated(r); }        // 被等待的链失败 → 标记终止（码透传）
        ResumeInline();                                 // 负载感知：内联或投递
    });
    if (!bOk) { Terminate(CStepResult::Failed(kStepStopped)); }  // 注册失败：同步终止并完成
}
```

- 注册成功后宏 `return`，协程让出线程；
- 链完成（可能很快，可能已完成的链走投递）→ 回调恢复协程；
- 恢复时若 `IsTerminated()`，宏在恢复点统一 `CompleteTerminated()` 结束。

### ResumeInline：负载感知的内联续接

```cpp
if (m_pExec->IsIdle() && detail::InlineDepth() < detail::kMaxInlineDepth)
{
    ++detail::InlineDepth();
    Resume();                 // 当前线程直接继续（省一次入队 + 唤醒）
    --detail::InlineDepth();
    return;
}
PostResume();                 // 队列有积压 / 深度超限：投递，保并行度 / 防爆栈
```

内联深度计数与**链的级联共用**（`detail::InlineDepth()`），因此链与协程互相嵌套时
仍受同一上限（64）保护。

## 6. 并行 await（CO_AWAIT_ALL）

```cpp
struct CAwaitAllGroup
{
    std::atomic<int> nPending;   // 剩余未完成链数
    std::atomic<int> bFailed;    // 是否已有链失败
    std::atomic<int> nCode;      // 首个失败码
};
```

`AwaitAll(nLine, chains...)` → `AwaitEach(pGroup, chains...)` 递归展开：

1. 每条链注册完成回调（回调捕获组状态与自持强引用）；
2. `OnAwaitDone`：失败时用 CAS 记录**首个失败码**，`nPending` 减 1；
3. `nPending` 归零 → 若组内有失败则标记终止 → `ResumeInline()` 恢复协程。

设计取舍：**等全部结束再恢复**（而不是首个失败立即恢复），避免提前释放仍在等待的
对象，也让失败码确定（首个失败）。

## 7. 终止与结束

| 函数 | 用途 |
| --- | --- |
| `MarkTerminated(r)` | 只标记（异步失败路径：等协程体走到恢复点统一出口） |
| `Terminate(r)` | 标记 + 立即 `Complete`（同步失败路径：协程体不会再被恢复） |
| `CompleteTerminated()` | 宏里的统一出口：以终止码完成协程 |
| `CompleteResult(r)` | `CO_RETURN(r)`：以指定结果完成 |
| `CompleteDone()` | `CO_RETURN_VOID()` / `CO_END()`：以成功完成 |

区分「标记」与「立即完成」的原因：异步失败发生在工作线程回调里，此时协程帧**没有被执行**，
必须先 `MarkTerminated` 再 `Resume`（让宏在恢复点处理）；而同步失败（注册不上 / 执行器停了）
发生在 `AwaitWait` 内部，直接 `Complete` 才能保证 `Get()` 不永久阻塞。

## 8. AsChain / Chain：复用链的核心

```cpp
CAsyncChain<TContext> AsChain() const
{ return CAsyncChain<TContext>::Make(m_pCore, m_pSegment); }

CAsyncChain<TContext> Chain(const StepFn& fnStep, const CSourceLoc& loc) const
{
    CAsyncChain<TContext> chain = CAsyncChain<TContext>::Make(m_pCore, nullptr);
    chain.Submit(fnStep, loc);          // 用协程的执行器与上下文起子链
    return chain;
}
```

- `Make` 是 `CAsyncChain` 的私有工厂，`CCoroutine<TContext>` 是其友元；
- 子链与协程共用 `m_pCore`（同一上下文 + 同一执行器句柄），因此
  「协程里看到的数据」与「子链写的数据」是同一份；
- 协程的 `m_pSegment` 同时是「协程完成状态」与「AsChain 的当前段」。

## 9. 与链的关系

```text
        CChainCore<TContext>（执行器句柄 + 共享上下文）
                 ▲                          ▲
                 │                          │
        CAsyncChain<TContext>        CCoroutine<TContext>
        （一层 = 一个段）              （完成状态 = 一个段）
                 │                          │
                 └──── chain.OnCompleted ←──┘（AsChain）
```

- 协程不引入独立的完成通知机制，也不引入值通道；
- 一次 await 的代价 = 起一条子链（投递首层）+ 一次完成回调 + 一次 Resume，
  基准见 `Benchmark/cases/CoroutineCase.cpp`（单层链约 0.6μs，协程一次 await 约 0.9μs）。

## 10. 测试覆盖

`Tests/test_async_chain.cpp` 的 `AsyncChainCoro_*` 用例：

| 用例 | 覆盖点 |
| --- | --- |
| `AsyncChainCoro_Sequential` | 顺序 await、上下文共享、轨迹顺序确定 |
| `AsyncChainCoro_Parallel` | `CO_AWAIT_ALL` 并行等待 |
| `AsyncChainCoro_AwaitFailed` | await 失败 → 终止，失败码透传，后续不执行 |
| `AsyncChainCoro_ReturnFailed` | `CO_RETURN(Failed(...))` 主动失败 |
| `AsyncChainCoro_Nested` | 子协程 `AsChain()` 嵌套 await |
| `AsyncChainCoro_NotStarted` | 未启动执行器 → `kStepStopped`，`Get()` 不阻塞 |
| `AsyncChainCoro_Restart` | 同一对象二次 `Start` 复用 |
| `AsyncChainCoro_CompletedCallback` | `AsChain().OnCompleted` 外部观察协程完成 |
