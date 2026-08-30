# 异步库 CAsyncExecutor / CTask — 使用文档

> 对应目录：`Common/Async`（命名空间 `common::async`）
> 实现细节见：[async-impl.md](async-impl.md)

## 1. 这是什么

`CAsyncExecutor` + `CTask` 是**无异常版（Option 风格）异步任务框架**：提交任务 → 链式变换 → 取结果，
全程**不需要 try/catch**。语义对标 Rust `Option`：

- **有值（Some）**：任务算出结果，沿链传播，下游继续；
- **无值（None）**：链终止（正常提前结束），下游全部跳过。

```cpp
namespace no = common::async;

no::CAsyncExecutor exec(2);   // 2 线程执行器
exec.Start();

no::CTaskResult<int> r = exec.Submit([]() { return 3; })
                             .Then([](int n) { return n * 2; })
                             .Then([](int n) { return n + 1; })
                             .Get();
// r.HasValue() == true，r.Value() == 7
```

> 错误不再是框架概念：**错误码是普通值**，由业务解释；框架只负责「有没有值」。
> 任务内抛异常被捕获并转为**无值终止**（原因 `kException`）。

## 2. 核心类型

| 类型 | 职责 |
|---|---|
| `CAsyncExecutor` | 执行器：`Start` / `Submit` / `Post` / `Stop` / `CoStart` |
| `CTask<TValue>` | 异步任务：`Then` / `Get` / `OnSuccess` / `OnNone` / `OnResult` |
| `CTaskResult<TValue>` | 任务结果：`HasValue` / `Value` / `ValueOr` / `Reason` |
| `CNoneTag` / `None` | 无值哨兵（`return no::None;` 终止链） |
| `CTaskEndReason` | 终止原因（仅调试）：`kEndCompleted` / `kEndNone` / `kNotStarted` / `kStopped` / `kException` |

## 3. 提交与链式

```cpp
// Submit 返回 CTask<TResult>；任务在工作线程执行。
no::CTask<int> t = exec.Submit([]() { return 10; });

// Then 链式续接：上游有值才执行，无值则终止传播。
t.Then([](int n) { return n * 3; })   // 普通值：传播
 .Then([](int n) -> no::CTaskResult<int> {
     if (n < 0) return no::None;      // 显式终止
     return n + 1;
 })
 .Then([](int n) { return n; });      // 上游终止 → 此步被跳过
```

### Then 变换函数的三种返回

| 返回类型 | 语义 |
|---|---|
| 普通值 `TNew` | 有值传播（`Then → Then → Get`） |
| `CTaskResult<TNew>` | 有值传播 / `return no::None` 终止 |
| `CTask<TNew>` | **扁平化 flatMap**：内部任务完成后转发其结果 |

## 4. 取结果与回调

```cpp
// 阻塞取结果（不抛异常，线程安全；多线程可同时 Get 同一任务）
no::CTaskResult<int> r = t.Get();
if (r.HasValue()) { int v = r.Value(); }
else { /* r.Reason() 区分 kEndNone / kException / kStopped ... */ }

// 回调（任务完成时在执行器线程异步触发）
t.OnSuccess([](const int& v) { /* 有值 */ });
t.OnNone([](no::detail::CTaskEndReason reason) { /* 终止 */ });
t.OnResult([](const no::CTaskResult<int>& result) { /* 有值/无值统一触发一次 */ });
```

- `ValueOr(def)`：有值返回值，无值返回默认；
- `void` 任务：`CTask<void>` 也支持 `Then`（变换函数无参）、`OnSuccess`（无参）；
- `Post(fn)`：提交无返回值任务（fire-and-forget）。

## 5. 生命周期

- **执行器析构 / Stop 后**：已投递、已链式任务仍安全完成（任务链通过**共享句柄**引用线程池）；
  新投递以无值（`kStopped` / `kNotStarted`）完成；
- 执行器未 `Start` 就 `Submit` → 任务立即以 `kNotStarted` 完成；
- `Stop()` 优雅关闭：置 `m_bStopped` 后等线程池任务排空。

## 6. 协程（CCoroutine）

`exec.CoStart<T>()` 启动无栈协程（见 [coroutine-usage.md](coroutine-usage.md)）。

## 7. 测试与更多

`Tests/test_common.cpp` 覆盖：链式、flatMap、None 传播、异常转无值、未启动/停止、
并发 Get、生命周期、多回调 fan-out 等；`Tests/test_perf.cpp` 为性能基准。
