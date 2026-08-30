# 无栈协程 CCoroutine — 实现文档

> 配套使用文档：[coroutine-usage.md](coroutine-usage.md)
> 源码：`Common/Async/Coroutine.h`（673 行，头文件实现）

## 1. 原理：Duff's device 状态机

无栈协程的挂起 = **`return` 让出线程**（栈弹出），恢复 = 重新调用 `Run()` 从上次挂起点继续。
为了记住「上次挂起点」，宏把协程体展开成 `switch (Step()) { case 0: ... case __LINE__: ... }`，
用 `__LINE__` 作为恢复点标签，步号存于成员 `m_hot.nStep`。

```cpp
// 协程体写：
CO_BEGIN();
CO_AWAIT_INTO(m_a, task1);
CO_RETURN(m_a);
CO_END();

// 宏展开后（示意）：
switch (Step()) { case 0:;
    AwaitInto(__LINE__, (task1), &m_a);   // 记录步号=该行 → 挂起 return
    return;
    case __LINE__:                        // 恢复：跳回这里
    if (IsTerminated()) { CompleteNone(Reason()); return; }
    CompleteResult(m_a); return;          // CO_RETURN
    }
    CompleteNone(); return;               // CO_END 兜底
```

- **跨 await 的局部变量必须放成员（帧）**：因为挂起时栈弹出、恢复时重入新建栈，只有成员跨重入存活；
- **每个宏独占一行**：`__LINE__` 作唯一恢复点标签，同行两个宏会冲突；
- `CO_BEGIN/CO_END` 必须保留：它们是 `switch` 骨架的开闭花括号。

## 2. CCoroutine 内部结构

```cpp
std::shared_ptr<detail::CTaskState<TValue> > m_pState; // 协程最终结果状态（复用异步库 CTaskState）
CAsyncExecutor* m_pExec;                               // 执行器指针（自动 Submit / Resume 调度）
std::weak_ptr<void> m_wpSelf;                          // 自持弱引用（生命周期加固）
CHotState m_hot;                                       // 热状态：nStep / bTerminated / reason 打包
```

`CHotState` 把 `step / terminated / reason` 三个原子打包在一起，减少跨线程迁移时的 cache line 数。

## 3. 生命周期加固（m_wpSelf）

```cpp
// CoStart：注入自持弱引用
pCoro->SetSelf(pCoro);            // m_wpSelf = pCoro（weak_ptr）

// PostResume：投递的 Resume 捕获自持强引用
std::shared_ptr<void> spSelf = m_wpSelf.lock();
m_pExec->Post([spSelf, this]() { Resume(); });   // 持有强引用 → 调用方提前释放 shared_ptr 也不悬垂
```

即：只要还有 Resume/回调在队列或执行中，协程对象就存活；全部执行完，最后一个强引用释放后析构。

## 4. 启动流程（CoStart → Start → PostResume）

```cpp
template <typename TCoroutine, typename... TArgs>
std::shared_ptr<TCoroutine> CAsyncExecutor::CoStart(TArgs&&... args)
{
    auto pCoro = std::make_shared<TCoroutine>(std::forward<TArgs>(args)...);
    pCoro->SetSelf(pCoro);   // 自持弱引用
    pCoro->Start(this);      // 绑定 + 复位 + 投递首次执行
    return pCoro;
}

void Start(CAsyncExecutor* pExec) { BindExecutor(pExec); Reset(); PostResume(); }
```

- `Reset`：重建结果状态、步号归 0、终止标志复位（**同一协程对象可重新 CoStart**）；
- `PostResume`：若执行器为空 / 无强引用 / 投递失败 → `Terminate(kStopped)`（安全终止，不悬垂）。

## 5. 任务规整：MakeTask

`CO_AWAIT*` 的 `expr` 三种传法经 `MakeTask` 统一成 `CTask<U>`：

```cpp
// 已提交 CTask<U>（TaskTraits::Kind == 1）→ 直接用
// 裸 lambda / 函数对象 → m_pExec->Submit(expr)（自动投递，免写 exec.Submit）
```

## 6. 顺序 await：RegisterTask + ResumeInline

`AwaitInto` / `AwaitWait` 先记录步号，再：

```cpp
template <typename U, typename TOnSuccess>
void RegisterTask(CTask<U> task, TOnSuccess onSuccess)
{
    std::shared_ptr<void> spSelf = m_wpSelf.lock();      // 强引用保活
    bool bOk = task.OnResult([spSelf, onSuccess, this](const CTaskResult<U>& result) {
        if (result.HasValue()) onSuccess(result.Value()); // 有值：落地(CWriteTarget) / 忽略(CIgnoreValue)
        else MarkTerminated(result.Reason());             // 无值 → 标记终止（原因透传）
        ResumeInline();                                   // 负载感知恢复
    });
    if (!bOk) Terminate(kStopped);                        // 任务已就绪但执行器不可用
}
```

**恢复路径（负载感知内联 ResumeInline）**：

```cpp
static thread_local int s_inlineDepth = 0;
if (m_pExec 有效 && !IsStopped() && m_pExec->IsIdle() && s_inlineDepth < 64)
{ ++s_inlineDepth; Resume(); --s_inlineDepth; return; }  // 线程池空闲 → 当前线程直接继续（省一次入队+唤醒）
PostResume();                                            // 有积压/深度超限 → 投递（保并行/防爆栈）
```

- 任务完成回调已运行在工作线程上，若线程池无积压（队列空）则**直接在此线程继续**协程体，
  减少「完成→入队→唤醒→恢复」的一次往返；有积压则投递保任务级并行度；
- `thread_local` 深度计数限制连续内联层数（防爆栈），超限回退投递（语义不变）。

## 7. 并行 await：CAwaitAllGroup

```cpp
struct CAwaitAllGroup {
    std::atomic<int> nPending; // 剩余未完成任务数
    std::atomic<int> bNone;    // 是否有任务无值
    std::atomic<int> nReason;  // 首个无值原因
};
```

- `AwaitAll`：`nPending = 任务数`，递归 `RegisterAllTask(pGroup, MakeTask(expr), CIgnoreValue())`；
- `AwaitAllInto`：参数成对 `(目标, 任务)`，递归 `RegisterAllTask(..., CWriteTarget<TDst>(&target))`；
- `RegisterAllTask`：`OnResult` 合并 OnSuccess/OnNone——有值落地/忽略、无值 `RecordNone`（CAS 记录**首个**原因），
  然后 `AllDone(pGroup)`；
- `AllDone`：`nPending.fetch_sub(1) == 1`（全部完成）→ 若 `bNone` 则 `MarkTerminated(原因)`，再 `ResumeInline()`。

`void` 任务在 `CO_AWAIT_ALL` 可用；`CO_AWAIT_ALL_INTO` 用 `static_assert` 要求参数成对，void 不支持（并行 void 请用 `CO_AWAIT_ALL`）。

## 8. Resume 与终止

```cpp
void Resume() { Run(); }   // 当前线程继续执行协程体，状态机从 m_nStep 恢复
```

- **终止判定由协程体宏完成**（`case` 处 `IsTerminated()` → `CompleteNone(Reason())`）：
  `Resume()` 不拦截，保证终止协程也能走到 `Complete`（`Get()` 不阻塞）；
- `Terminate(reason)` = `MarkTerminated + CompleteNone`（无值完成）；
- 执行器 `Stop` 后：`PostResume` 投递失败 → `Terminate(kStopped)`，已挂起协程安全终止。

## 9. AsTask：协程作为可 await 任务

```cpp
CTask<TValue> AsTask() { return m_pExec->AdoptState<TValue>(m_pState); }
```

复用本协程的结果状态，返回绑定执行器句柄的 `CTask`——外层协程 `CO_AWAIT(m_r, child.AsTask())`
即可 await 子协程；子协程完成（`CO_RETURN`/终止）后外层恢复。须先 `CoStart` 绑定执行器。

## 10. 与异步库的关系

| 能力 | 提供方 |
|---|---|
| 任务状态 / 完成 / 续接 / Wait | `CTaskState`（AsyncExecutor.h） |
| 提交 / 执行器句柄 / 生命周期 | `CAsyncExecutor` / `CExecutorHandle` |
| 顺序代码 ↔ 状态机 | `CCoroutine` 宏（Coroutine.h） |

协程不重造线程调度：挂起与恢复都通过 `CAsyncExecutor::Post` 投递到同一线程池；`CTaskState` 复用，
保证 `Get()` 语义、并发 Get、终止原因与 `CTask` 完全一致。

## 11. 测试覆盖

`Tests/test_coroutine.cpp`：顺序 await、值传递、终止（异常/None）、并行 await、嵌套、
生命周期（Stop 后安全终止）、复用（同一对象二次 CoStart）、并发、压力（万级协程 RSS）；
`Tests/test_perf.cpp`：性能基准（基线 / 顺序 / 并行 / 嵌套对比）。
