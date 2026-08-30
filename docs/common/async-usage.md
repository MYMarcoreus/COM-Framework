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

### CTaskResult<T>：一张「有值或无值」的结果单

- `HasValue()`：有值用 `Value()` 取；无值 = 链终止，只有终止原因 `Reason()`（仅调试）；
- 构造：`CTaskResult<int> ok(42);`（有值，隐式）、`CTaskResult<int> none;`（默认 = 无值）、
  `CTaskResult<int>(no::None);`（显式无值）；
- 便捷：`if (r)`（`operator bool`）、`r.ValueOr(-1)`（无值给默认值）；
- **实现**：值**内联存储**（对标 `std::optional` / Rust `Option`）——无堆分配、无引用计数；
  结果是深拷贝，需「默认构造 + 可拷贝」；move-only 类型（如 `std::unique_ptr`）不支持，
  可改用 `std::shared_ptr` 包裹。

### 终止原因 CTaskEndReason（仅调试）

| 值 | 含义 |
|---|---|
| `kEndCompleted=0` | 有值（正常完成） |
| `kEndNone=1` | 业务返回 `no::None`（正常提前终止） |
| `kNotStarted=2` | 执行器没 `Start` 就 Submit |
| `kStopped=3` | 执行器已停止，再投递被拒 |
| `kException=4` | 任务 / 变换抛异常（框架捕获转无值） |

> `Reason()` 不是错误码，只是诊断标签。业务想区分「为什么终止」，请自己在链里传值
> （如返回 `CTaskResult<enum>`），框架不替你做业务判断。

## 3. 提交与链式

```cpp
no::CTask<int> t = exec.Submit([]() { return 10; });

t.Then([](int n) { return n * 3; })
 .Then([](int n) -> no::CTaskResult<int> { if (n < 0) return no::None; return n + 1; })
 .Then([](int n) { return n; });   // 上游终止 → 此步被跳过
```

### Then 变换函数的三种返回

| 返回类型 | 语义 |
|---|---|
| 普通值 `TNew` | 有值传播（`Then → Then → Get`） |
| `CTaskResult<TNew>` | 有值传播 / `return no::None` 终止 |
| `CTask<TNew>` | **扁平化 flatMap**：内部任务完成后转发其结果 |

```cpp
// flatMap：变换本身也是异步（如查数据库）
exec.Submit([]() { return 3; })
    .Then([&exec](int n) { return exec.Submit([n]() { return n * n; }); })  // 返回任务 → 自动平铺
    .Then([](int n) { return "平方 = " + std::to_string(n); })
    .Get();
```

### CTask<void> 也支持 Then（变换无参）

```cpp
no::CTaskResult<int> r =
    exec.Submit([]() { /* 干点事 */ })  // CTask<void>
        .Then([]() { return 42; })        // void → int（无参数）
        .Then([](int n) { return n + 8; }) // 继续正常链
        .Get();
// r.Value() == 50
```

- `CTaskResult<void>::HasValue()` = 是否完成（无值 → 终止）；
- `OnSuccess` 回调无参；`OnNone` 回调收 `Reason()`；void 上游返回 `no::None` 同样终止下游。

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

- `Post(fn)`：提交无返回值任务（fire-and-forget）；
- 任务**已完成**时注册的回调同样投递到执行器**异步执行**（执行器不可用 → 回调不执行，不退回同步）。

## 5. 无值终止模型

| 无值来源 | 表现 |
|---|---|
| 任务函数 `throw` | 框架捕获 → 无值（`kException`） |
| 业务主动终止 | `return no::None;`（`kEndNone`） |
| 执行器未启动 | 无值（`kNotStarted`） |
| 执行器已停止 | 无值（`kStopped`） |

> 框架不保留异常文本、不定义错误码；具体原因业务自己在任务函数里记录日志或返回值传递。

## 6. 线程模型

1. **任务函数和续接在工作线程执行**——别在工作线程碰主线程的私有数据（要加锁或用弱引用）；
2. **任务已完成时再注册回调** → 回调**投递到执行器异步执行**（与 JS/C# 一致）——回调在哪个线程不固定，别做线程假设；
3. **各 `Then` 步不固定在同一线程**：每个变换会重新投递到线程池，由任意空闲工作线程执行；
   保证的是链式顺序（后一步在前一步完成后），不保证执行线程（无线程亲和）。

## 7. 生命周期

- **执行器析构 / Stop 后**：已投递、已链式任务仍安全完成（任务链通过**共享句柄**引用线程池）；
  新投递以无值（`kStopped` / `kNotStarted`）完成；
- 执行器未 `Start` 就 `Submit` → 任务立即以 `kNotStarted` 完成；
- `Stop()` 优雅关闭：置 `m_bStopped` 后等线程池任务排空。

## 8. 协程（CCoroutine）

`exec.CoStart<T>()` 启动无栈协程（见 [coroutine-usage.md](coroutine-usage.md)）。

## 9. 常见用法速查

```cpp
// ① 简单提交
no::CAsyncExecutor exec(2); exec.Start();
no::CTaskResult<int> r = exec.Submit([]() { return 42; }).Get();

// ② 链式
auto r2 = exec.Submit([]() { return 3; })
              .Then([](int n) { return n * 2; })
              .Then([](int n) { return n + 1; }).Get();

// ③ flatMap（变换返回任务）
auto r3 = exec.Submit([]() { return 3; })
              .Then([&exec](int n) { return exec.Submit([n]() { return n * n; }); }).Get();

// ④ 回调（fire-and-forget）
no::CTask<int> t = exec.Submit([]() { return 7; });
t.OnSuccess([](const int& v) { /* 有值 */ });
t.OnNone([](no::detail::CTaskEndReason) { /* 无值终止 */ });

// ⑤ void 任务（也支持 Then）
exec.Submit([]() { /* 干点事 */ }).Then([]() { return 1; }).Get();

// ⑥ 手动构造结果
auto ok   = no::CTaskResult<int>(1);   // 有值
auto none = no::CTaskResult<int>();    // 无值

// ⑦ 中途终止（Option 风格）
exec.Submit([]() { return -5; })
    .Then([](int n) -> no::CTaskResult<int> {
        if (n < 0) return no::None;    // 终止
        return n * 2;                  // 传播
    })
    .Then([](int n) { return n + 1; }).Get();

exec.Stop(); // 优雅关闭：等已提交任务完成
```

## 10. 测试与更多

`Tests/test_common.cpp` 覆盖：链式、flatMap、None 传播、异常转无值、未启动/停止、并发 Get、
生命周期、多回调 fan-out 等；`Tests/test_perf.cpp` 为性能基准。
完整可运行示例见 `examples/main.cpp`。
