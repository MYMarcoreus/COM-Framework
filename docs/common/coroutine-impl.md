# 无栈协程 CCoroutine — 实现文档

> 对应文件：`Common/Coroutine/Coroutine.h`（命名空间仍是 `common::async`）
> 使用方式见：[coroutine-usage.md](coroutine-usage.md) ｜ promise 实现见：[async-impl.md](async-impl.md)

## 1. 原理：Duff's device 状态机

无栈协程把「函数体」编译成带恢复点的状态机：每个 `CO_AWAIT` 处保存恢复点（`__LINE__`）
并 `return` 让出线程，promise settled 后从该 `case` 继续执行。

```cpp
#define CO_BEGIN()  \
    switch (Step()) \
    {               \
        case 0:;

#define CO_AWAIT(expr)           \
    AwaitWait(__LINE__, (expr)); \
    \  // 注册 settled 通知（挂起）
return;
\  // 让出线程
    case __LINE__:
\  // ← 恢复点
    if (IsTerminated())
{
    CompleteTerminated();
    return;
}
```

要点：

- `switch (Step())` 让状态机回到上次恢复点；`case 0` 是首次进入（`CO_BEGIN`）；
- 每个 `CO_AWAIT` 独占一行，因为 `__LINE__` 就是它的标签；
- 恢复时若已终止（被等待的 promise 被拒绝），统一走 `CompleteTerminated()`。

## 2. 内部结构

```cpp
template <typename TContext>
class CCoroutine
{
    std::shared_ptr<detail::CPromiseCore<TContext> > m_pCore;  // 执行器句柄 + 共享上下文
    std::shared_ptr<detail::CPromiseState> m_pSegment;         // 协程完成状态（AsPromise 暴露）
    CAsyncExecutor* m_pExec;                                   // 调度（Resume + 子 promise 投递）
    std::weak_ptr<void> m_wpSelf;                              // 自持弱引用（生命周期加固）
    CHotState m_hot;                                           // 步号 / 终止标志 / 拒绝码
};
```

`CHotState` 把三个热字段打包相邻，减少跨线程迁移时的 cache line 数：

```cpp
struct CHotState
{
    std::atomic<int> nStep;         // 状态机步号（恢复点）
    std::atomic<bool> bTerminated;  // await 到拒绝 → 终止
    std::atomic<int> nCode;         // 终止拒绝码
};
```

协程的**完成状态直接复用 promise 状态**（`detail::CPromiseState`）—— 因此 `AsPromise()`
只要把 `(m_pCore, m_pSegment)` 包成 promise 句柄，协程就自然成为可 await 的对象，
不需要第二套「完成通知」实现。

## 3. 生命周期加固（m_wpSelf）

所有会「稍后回来」的地方都先 `m_wpSelf.lock()` 取强引用，再捕获进回调：

```cpp
std::shared_ptr<void> spSelf = m_wpSelf.lock();
promise.OnSettled([spSelf, this](CPromiseResult r)
{
    ...
});  // 回调期间对象保活
m_pExec->Post([spSelf, this]()
{
    Resume();
});
```

因此调用方即使提前释放 `CoStart` 返回的 `shared_ptr`，协程对象也会存活到最后一个
Resume / await 回调执行完毕（不悬垂）。

## 4. 启动流程

```text
exec.CoStart<TCoroutine>(args...)
    ├── make_shared<TCoroutine>(args...)     创建（构造时建 m_pCore 与初始 m_pSegment）
    ├── pCoro->SetSelf(pCoro)                注入自持弱引用
    └── pCoro->Start(this)
             ├── BindExecutor(pExec)         m_pExec = pExec；把执行器句柄写入 m_pCore
             ├── Reset()                     新建 m_pSegment；步号 / 终止标志复位
             └── PostResume()                投递首次 Resume（执行器不可用 → 立即以 kStopped 结束）
```

`Reset()` 让同一协程对象可以重新 `Start`（重复使用）。

## 5. 顺序 await（AwaitWait）

```cpp
void AwaitWait(int nLine, const CPromise<TContext>& promise)
{
    m_hot.nStep.store(nLine);  // 记恢复点
    std::shared_ptr<void> spSelf = m_wpSelf.lock();
    bool bOk = promise.OnSettled([spSelf, this](CPromiseResult r)
    {
        if (r.IsRejected())
        {
            MarkTerminated(r);
        }                // 被等待的 promise 被拒绝 → 标记终止
        ResumeInline();  // 线程亲和 + 负载感知：内联或投递
    });
    if (!bOk)
    {
        Terminate(CPromiseResult::Reject(kStopped));
    }  // 注册失败：同步终止并 settle
}
```

- 注册成功后宏 `return`，协程让出线程；
- promise settled（可能很快，也可能是已 settled 的 promise 走投递）→ 回调恢复协程；
- 恢复时若 `IsTerminated()`，宏在恢复点统一 `CompleteTerminated()` 结束。

### ResumeInline：线程亲和 + 负载感知的内联续接

```cpp
if (m_pExec->IsInExecutorThread()                            // ① 线程亲和：必须在本协程自己的执行器线程上
    && detail::ShouldInline(kAffinityChain, m_pExec->Handle(), /* bRequireIdle = */ true)  // ② 无积压 + 深度未超限
{
    ++detail::InlineDepth();
    Resume();  // 在当前线程直接继续（省一次入队 + 唤醒）
    --detail::InlineDepth();
    return;
}
PostResume();                 // 跨执行器 / 队列有积压 / 深度超限：投递，回本执行器 / 保并行度 / 防爆栈
```

① 是 2026-09-11 的线程亲和（改进 A）：`CO_AWAIT` 等别的模块的 promise 时，回调在被调模块线程上跑，
此时若直接 `Resume()`，协程体就跑到别人模块的线程上了；加①后协程体**始终在自己的执行器线程**上续跑
（验收：`Tests/test_async_affinity.cpp` 的 `Affinity_CoroutineResumesOnOwnExecutor`）。

内联深度计数与 **promise 的级联共用**（`detail::InlineDepth()`），因此 promise 与协程
互相嵌套时仍受同一上限（64）保护。

## 6. 并行 await（CO_AWAIT_ALL）

```cpp
struct CAwaitAllGroup
{
    std::atomic<int> nPending;   // 剩余未完成 promise 数
    std::atomic<int> bRejected;  // 是否已有 promise 被拒绝
    std::atomic<int> nCode;      // 首个拒绝码
};
```

`AwaitAll(nLine, promises...)` → `AwaitEach(pGroup, promises...)` 递归展开：

1. 每条 promise 注册 settled 通知（回调捕获组状态与自持强引用）；
2. `OnAwaitDone`：被拒绝时用 CAS 记录**首个拒绝码**，`nPending` 减 1；
3. `nPending` 归零 → 若组内有拒绝则标记终止 → `ResumeInline()` 恢复协程。

设计取舍：**等全部结束再恢复**（而不是首个拒绝立即恢复），避免提前释放仍在等待的对象，
也让拒绝码确定（首个）。

## 7. 终止与结束

| 函数 | 用途 |
| --- | --- |
| `MarkTerminated(r)` | 只标记（异步拒绝路径：等协程体走到恢复点统一出口） |
| `Terminate(r)` | 标记 + 立即 `Settle`（同步失败路径：协程体不会再被恢复） |
| `CompleteTerminated()` | 宏里的统一出口：以终止码 settle 协程 |
| `CompleteResult(r)` | `CO_RETURN(r)`：以指定结果 settle |
| `CompleteDone()` | `CO_RETURN_VOID()` / `CO_END()`：以兑现 settle |

区分「标记」与「立即 settle」的原因：异步拒绝发生在工作线程回调里，此时协程帧**没有被执行**，
必须先 `MarkTerminated` 再 `Resume`（让宏在恢复点处理）；而同步失败（注册不上 / 执行器停了）
发生在 `AwaitWait` 内部，直接 `Settle` 才能保证 `Await()` 不永久阻塞。

## 8. AsPromise / NewPromise：复用 promise 的核心

```cpp
CPromise<TContext> AsPromise() const
{
    return CPromise<TContext>::Make(m_pCore, m_pSegment);
}

CPromise<TContext> NewPromise(const ThenHandler& fnHandler, const CSourceLoc& loc) const
{
    return CPromise<TContext>::StartChain(m_pCore, fnHandler, loc);
}
```

- `Make` / `StartChain` 都是 `CPromise` 的私有工厂，`CCoroutine<TContext>` 是其友元；
- 子 promise 与协程共用 `m_pCore`（同一上下文 + 同一执行器句柄），因此
  「协程里看到的数据」与「子 promise 写的数据」是同一份；
- `StartChain` 与 `exec.NewPromise(spCtx, handler)` 是**同一条起链路径**（建首层状态 + 强制投递首层），
  只是这里复用了协程已有的核心（而不是新建一个）；
- 协程的 `m_pSegment` 同时是「协程完成状态」与「AsPromise 的当前状态」。

## 9. 与 promise 的关系

```text
        CPromiseCore<TContext>（执行器句柄 + 共享上下文）
                 ▲                          ▲
                 │                          │
        CPromise<TContext>          CCoroutine<TContext>
        （一层 = 一个状态）            （完成状态 = 一个状态）
                 │                          │
                 └──── promise.OnSettled ←──┘（AsPromise）
```

- 协程不引入独立的完成通知机制，也不引入值通道；
- 一次 await 的代价 = 起一条子 promise（投递首层）+ 一次 settled 通知 + 一次 Resume，
  基准见 `Benchmark/cases/CoroutineCase.cpp`（单层 promise 约 0.6μs，协程一次 await 约 0.9μs）。

## 10. 测试覆盖

`Tests/test_async_chain.cpp` 的 `Coro_*` 用例：

| 用例 | 覆盖点 |
| --- | --- |
| `Coro_Sequential` | 顺序 await、上下文共享、轨迹顺序确定 |
| `Coro_Parallel` | `CO_AWAIT_ALL` 并行等待 |
| `Coro_AwaitRejected` | await 被拒绝 → 终止，拒绝码透传，后续不执行 |
| `Coro_ReturnRejected` | `CO_RETURN(Reject(...))` 主动拒绝 |
| `Coro_Nested` | 子协程 `AsPromise()` 嵌套 await |
| `Coro_NotStarted` | 未启动执行器 → `kStopped`，`Await()` 不阻塞 |
| `Coro_Restart` | 同一对象二次 `Start` 复用 |
| `Coro_OnSettledCallback` | `AsPromise().OnSettled` 外部观察协程完成 |
