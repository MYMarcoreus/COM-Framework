# 无栈协程 CCoroutine — 使用文档

> 对应目录：`Common/Async/Coroutine.h`（命名空间 `common::async`）
> 实现细节见：[coroutine-impl.md](coroutine-impl.md)

## 1. 这是什么

`CCoroutine<TValue>` 是**无栈协程**：基于 `CAsyncExecutor` + `CTask`，用宏把「C# async/await」
风格的顺序代码展开为 **Duff's device 状态机**。

- 无异常契约：与 `CTask` 的 Option 风格一致，`await` 到无值（业务 `None` / 任务异常）即终止（原因透传）；
- 无栈：挂起 = `return` 让出线程（线程回线程池），恢复 = 执行器投递 `Resume`；
- 生命周期加固：框架内部 Resume/回调捕获自持强引用，调用方提前释放 `shared_ptr` 也不悬垂。

## 2. C# 对照

| C# | 本项目 |
|---|---|
| `await task;` | `CO_AWAIT(task);`（纯等待，主版本） |
| `int a = await task;` | `CO_AWAIT_INTO(m_a, task);`（落地值，m_a 为帧成员） |
| `await Task.WhenAll(t1, t2);` | `CO_AWAIT_ALL(t1, t2);` |
| 并行并落地值 | `CO_AWAIT_ALL_INTO(m_a, t1, m_b, t2);` |
| `return value;` | `CO_RETURN(value);` |
| 失败处理 | `await` 无值 → 整体终止（`Reason()` 区分） |

## 3. 快速上手

```cpp
// 1) 定义协程类：继承 CCoroutine<TValue>，实现 Run()（用协程宏写顺序代码）。
class CMyCoro : public common::async::CCoroutine<int>
{
public:
    CMyCoro() : m_nA(0), m_nB(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_nA, []() { return 3; });            // 落地值：裸 lambda 自动 Submit
        CO_AWAIT_INTO(m_nB, [this]() { return m_nA * 2; }); // 捕获 this 读帧变量
        CO_RETURN(m_nA + m_nB);                             // return 值（正常结束）
        CO_END();                                           // 兜底：无值终止
    }

private:
    int m_nA; // 落地值目标 = 派生类成员（帧）
    int m_nB;
};

// 2) 启动并取结果（同一执行器）。
common::async::CAsyncExecutor exec(2);
exec.Start();
std::shared_ptr<CMyCoro> pCoro = exec.CoStart<CMyCoro>();
common::async::CTaskResult<int> r = pCoro->Get();   // 阻塞取结果（不抛异常）
if (r.HasValue()) { /* 有值 */ }
else { /* 终止，r.Reason() 区分 kEndNone / kException / kStopped */ }
exec.Stop();
```

## 4. 宏参考

| 宏 | 说明 |
|---|---|
| `CO_BEGIN()` | 状态机入口（Duff's device 的 `switch` 开头，每协程体一对） |
| `CO_AWAIT(expr)` | **主版本**：纯等待任意任务（void / 非 void），忽略返回值；值经外部共享 `shared_ptr` 传递 |
| `CO_AWAIT_INTO(target, expr)` | 落地值（非 void 专用）：await 任务并写入 target，恢复后直接用 |
| `CO_AWAIT_ALL(任务...)` | 并行纯等待多任务（忽略返回值），全部完成恢复 |
| `CO_AWAIT_ALL_INTO(目标, 任务, ...)` | 并行落地值：结果写入各自目标，全部完成恢复 |
| `CO_RETURN(expr)` | 协程正常结束（有值） |
| `CO_RETURN_VOID()` | 协程正常结束（void 完成） |
| `CO_END()` | 状态机结尾（兜底无值终止） |

`expr`（任务）支持三种，自动分派：
- **已提交任务** `CTask<U>`（可链式 `Then`/flatMap）；
- **裸 lambda / 函数对象**：自动 `Submit` 到执行器（免写 `exec.Submit`）；
- **子协程** `child.AsTask()`：await 嵌套协程。

`CO_AWAIT_INTO` 的 `target` 可为任意可写左值：成员（`m_nA`）、解引用（`*sp`）、成员访问（`sp->field`）。

## 5. 数据传递（无栈协程的固有约束）

**跨 await 的变量不能用函数内局部变量**（挂起时 `Run()` 栈弹出、恢复时重入重建）。推荐：

**① 共享 shared_ptr（配合主版本 CO_AWAIT）**

```cpp
std::shared_ptr<int> m_spVal;   // 帧成员，协程常驻
// 协程内：任务 lambda 捕获共享对象并写入，纯等待
CO_AWAIT([this]() { *m_spVal = 42; });
CO_RETURN(*m_spVal);
```

**② 落地值（CO_AWAIT_INTO）→ 帧成员 / 解引用**

```cpp
int m_nPort;                                   // 帧成员，跨 await 存活
CO_AWAIT_INTO(m_nPort, []() { return 读配置(); });
std::shared_ptr<Result> m_sp;
CO_AWAIT_INTO(m_sp->field, []() { return 42; });   // target = sp->field
```

**③ 嵌套子协程 → shared_ptr 子协程 + `AsTask()`**

```cpp
std::shared_ptr<CChildCoro> m_spChild;   // 子协程（已 CoStart）
CO_AWAIT_INTO(m_nR, m_spChild->AsTask());     // await 子协程结果
```

## 6. 并行 await

```cpp
// ① 纯等待并行（忽略返回值，值经共享 shared_ptr 传递；任务可为非 void，返回值被忽略）
std::shared_ptr<Result> m_sp;   // 任务 lambda 捕获写结果
CO_AWAIT_ALL([this]() { 查服务A(*m_sp); },
             [this]() { 查服务B(*m_sp); },
             []() { return 无关返回值(); });   // 非 void 任务，返回值忽略
CO_RETURN(m_sp->load());

// ② 并行落地值（成对：目标, 任务；目标须为左值）
int m_a; int m_b;
CO_AWAIT_ALL_INTO(m_nA, []() { return 查服务A(); },
                  m_nB, []() { return 查服务B(); });
CO_RETURN(聚合(m_nA, m_nB));
```

- 全部任务完成 → 恢复（纯等待）或各自写入目标后恢复（落地值）；
- **任一任务无值** → 协程整体终止（原因透传，与顺序 await 一致）；
- `CO_AWAIT_ALL_INTO` 的 void 任务暂不支持（并行 void 请用 `CO_AWAIT_ALL`）。

## 7. 终止语义（Option 风格）

| 场景 | 结果 |
|---|---|
| 业务 `return no::None`（变换函数返回 `CTaskResult`） | 无值，`kEndNone` |
| 任务 / 变换抛异常 | 无值，`kException`（框架捕获） |
| 执行器未启动 / 已停止 | 无值，`kStopped` / `kNotStarted` |
| await 到无值（任一） | 协程终止，`Reason()` 透传 |

## 8. 生命周期

- `CoStart` 返回 `shared_ptr<TCoroutine>`，**调用方须持有到完成以 `Get()` 取结果**；
- 框架内部 `Resume` / 续接回调**捕获自持强引用**：即使调用方提前释放 `shared_ptr`，
  协程对象仍存活到最后一个 `Resume` 执行完毕（不悬垂）；
- 执行器须存活于协程生命周期（协程绑定 `CAsyncExecutor*`）；
- 执行器 `Stop` / 析构后，已挂起协程安全以 `kStopped` 终止。

## 9. 限制

- **跨 await 变量必须存帧**（成员 / shared_ptr 成员 / 子协程），不能用函数内局部变量；
- `CO_BEGIN()` / `CO_END()` 必须保留（Duff's device 的 `switch` 骨架，无法隐藏）；
- **每个协程宏独占一行**（`__LINE__` 作恢复点标签，同一行两个宏冲突）；
- `CO_AWAIT_ALL_INTO` 目标须为左值；并行 void 请用 `CO_AWAIT_ALL`（纯等待）；
- 无栈协程无法做到「真·局部变量自动保留」——若需要（`int x = await task` 且 x 为纯局部），
  需切换到**有栈协程**（路线 B）或 **C++20 原生协程**。

## 10. 与 CTask 的关系

`CCoroutine` 复用 `CTask` 生态：
- 任务（`CTask<U>`）是 await 的载体；`AsTask()` 把协程暴露为 `CTask`（可被外层 await，也可链式）；
- 结果统一为 `CTaskResult<T>`（`HasValue()` / `Value()` / `ValueOr()` / `Reason()`）；
- 共享 `CAsyncExecutor`（线程池）调度，语义与 `CTask::Then` 链一致。

## 11. 测试

`Tests/test_coroutine.cpp`：顺序 await、值传递、终止（异常/None）、并行 await、嵌套、生命周期、
复用（同一对象二次 CoStart）、并发、压力（万级协程 RSS）；`Tests/test_perf.cpp`：性能基准。
