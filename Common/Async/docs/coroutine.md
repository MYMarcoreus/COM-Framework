# 无栈协程 CCoroutine 使用指南（common::async）

## 1. 这是什么？

`CCoroutine` 是 `common::async` 提供的一套**无栈协程**：基于 `CAsyncExecutor` + `CTask`，
用宏把「C# async/await」风格的顺序代码展开为 **Duff's device 状态机**。

- 无异常契约：与 `CTask` 的 Option 风格一致，`await` 到无值（业务 `None` / 任务异常）
  即**终止**（原因透传），不向调用方抛异常；
- 无栈：挂起 = `return` 让出线程（线程回线程池），恢复 = 执行器投递 `Resume`；
- 生命周期加固：框架内部 Resume/回调捕获自持强引用，调用方提前释放 `shared_ptr` 也不悬垂。

## 2. C# 对照

| C# | 本项目 |
|---|---|
| `int a = await task;` | `CO_AWAIT(m_a, task);`（m_a 为帧成员） |
| `int a = await Foo();` | `CO_AWAIT(m_a, []() { return Foo(); });` |
| `await voidTask;` | `CO_AWAIT_VOID(voidTask);` |
| `await Task.WhenAll(t1, t2);` | `CO_AWAIT_ALL(m_a, t1, m_b, t2);` |
| `return value;` | `CO_RETURN(value);` |
| 失败处理（异常 / 降级） | `await` 无值 → 整体终止（`Reason()` 区分） |

## 3. 快速上手

```cpp
// 1. 定义协程类：继承 CCoroutine<TValue>，实现 Run()（用协程宏写顺序代码）。
class CMyCoro : public common::async::CCoroutine<int>
{
public:
    CMyCoro() : m_nA(0), m_nB(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(m_nA, []() { return 3; });            // 裸 lambda 自动 Submit
        CO_AWAIT(m_nB, [this]() { return m_nA * 2; }); // 捕获 this 读帧变量
        CO_RETURN(m_nA + m_nB);                        // return 值（正常结束）
        CO_END();                                      // 兜底：无值终止
    }

private:
    int m_nA; // 跨 await 变量 = 派生类成员（帧）
    int m_nB;
};

// 2. 启动并取结果（同一执行器）。
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
| `CO_BEGIN()` | 状态机入口（Duff's device 的 `switch` 开头，每个协程体一对）。 |
| `CO_AWAIT(target, expr)` | await 任务并写入 target；挂起，恢复后 target 直接用。 |
| `CO_AWAIT_VOID(expr)` | await void 任务（完成后恢复）。 |
| `CO_AWAIT_WAIT(expr)` | 纯等待任意任务（返回值忽略），值由共享数据传递。 |
| `CO_AWAIT_ALL(目标1, 任务1, 目标2, 任务2, ...)` | 并行 await 多任务，全部完成恢复。 |
| `CO_RETURN(expr)` | 协程正常结束（有值）。 |
| `CO_RETURN_VOID()` | 协程正常结束（void 完成）。 |
| `CO_END()` | 状态机结尾（兜底无值终止）。 |

`expr`（任务）支持三种传法，自动分派：
- **已提交任务** `CTask<U>`（可链式 `Then`/`flatMap`）：`CO_AWAIT(m_r, exec.Submit(fn).Then(...))`
- **裸 lambda / 函数对象**：自动 `Submit` 到执行器执行（免写 `exec.Submit`）
- **子协程** `child.AsTask()`：await 嵌套协程

`target` 可为任意可写左值：成员（`m_nA`）、解引用（`*sp`）、成员访问（`sp->field`）。

## 5. 数据传递（三种模式）

无栈协程的固有约束：**跨 await 的变量不能用函数内局部变量**（挂起时 `Run()` 栈弹出，
恢复时重入重建）。三种可行模式：

**① 简单值 → 派生类成员（帧）**

```cpp
int m_nPort;   // 帧成员，跨 await 存活
CO_AWAIT(m_nPort, []() { return 读配置(); });
```

**② 动态数据 → shared_ptr 成员（含纯等待 `CO_AWAIT_WAIT`）**

```cpp
std::shared_ptr<Result> m_sp;   // shared_ptr 随协程常驻（可外部传入，多协程共享）

// 方式 A：target = sp->field（编译期类型检查 + 值落地）
CO_AWAIT(m_sp->field, []() { return 42; });

// 方式 B：CO_AWAIT_WAIT 纯等待 —— 任务捕获 sp 直接写结果，await 不取返回值
CO_AWAIT_WAIT([this]() { return 查服务(m_sp); });   // 非 void，值走 sp
CO_AWAIT_WAIT([this]() { m_sp->聚合(); });          // void
```

**③ 嵌套子协程 → shared_ptr 子协程 + `AsTask()`**

```cpp
std::shared_ptr<CChildCoro> m_spChild;   // 子协程（已 CoStart）
CO_AWAIT(m_nR, m_spChild->AsTask());     // await 子协程结果
```

## 6. 并行 await（CO_AWAIT_ALL）

```cpp
class CAllCoro : public common::async::CCoroutine<int>
{
public:
    CAllCoro() : m_nA(0), m_nB(0), m_nC(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_ALL(m_nA, []() { return 查服务A(); },   // 并行提交，全部完成恢复
                     m_nB, []() { return 查服务B(); },
                     m_nC, []() { return 查服务C(); });
        CO_RETURN(聚合(m_nA, m_nB, m_nC));
        CO_END();
    }

private:
    int m_nA, m_nB, m_nC;
};
```

- 全部任务有值 → 各自写入目标后恢复；
- **任一任务无值** → 协程整体终止（原因透传，与顺序 await 一致）；
- void 任务暂不支持（并行 void 请用多个 `CO_AWAIT_VOID`）。

## 7. 生命周期

- `CoStart` 返回 `shared_ptr<TCoroutine>`，**调用方须持有到完成以 `Get()` 取结果**；
- 框架内部 `Resume` / 续接回调**捕获自持强引用**：即使调用方提前释放 `shared_ptr`，
  协程对象仍存活到最后一个 `Resume` 执行完毕（不悬垂）；
- 执行器须存活于协程生命周期（协程绑定 `CAsyncExecutor*`）；
- 执行器 `Stop` / 析构后，已挂起协程安全以 `kStopped` 终止。

## 8. 终止语义（Option 风格）

| 场景 | 结果 |
|---|---|
| 业务 `return no::None`（变换函数返回 `CTaskResult`） | 无值，`kEndNone` |
| 任务 / 变换抛异常 | 无值，`kException`（框架捕获） |
| 执行器未启动 / 已停止 | 无值，`kStopped` / `kNotStarted` |
| await 到无值（任一） | 协程终止，`Reason()` 透传 |

## 9. 限制

- **跨 await 变量必须存帧**（成员 / shared_ptr 成员 / 子协程），不能用函数内局部变量；
- `CO_BEGIN()` / `CO_END()` 必须保留（Duff's device 的 `switch` 骨架，无法隐藏）；
- **每个协程宏独占一行**（`__LINE__` 作恢复点标签，同一行两个宏冲突）；
- `CO_AWAIT_ALL` 目标须为左值；void 任务用 `CO_AWAIT_VOID`；
- 无栈协程无法做到「真·局部变量自动保留」——若需要（`int x = await task` 且 x 为纯局部），
  需切换到**有栈协程**（路线 B）或 **C++20 原生协程**。

## 10. 与 CTask 的关系

`CCoroutine` 复用 `CTask` 生态：
- 任务（`CTask<U>`）是 await 的载体；`AsTask()` 把协程暴露为 `CTask`（可被外层 await，也可链式）；
- 结果统一为 `CTaskResult<T>`（`HasValue()` / `Value()` / `ValueOr()` / `Reason()`）；
- 共享 `CAsyncExecutor`（线程池）调度，语义与 `CTask::Then` 链一致。
