# 无栈协程 CCoroutine — 使用文档

> 对应文件：`Common/Coroutine/Coroutine.h`（命名空间仍是 `common::async`：协程与 promise 共用同一套模型）
> 实现细节见：[coroutine-impl.md](coroutine-impl.md) ｜ promise 见：[async-usage.md](async-usage.md)

## 1. 这是什么

promise 负责**编排**（then / catch / finally 串起来，失败即停）；协程负责**顺序化** ——
用顺序代码 await 多条 promise，替代回调嵌套。

两者共用同一套模型：

- 协程持有一个共享上下文 `std::shared_ptr<TContext>`（与它起的子 promise 同一实例）；
- await 的对象是**promise**（包含子协程 `AsPromise()` 暴露的 promise）；
- await 只告知兑现 / 拒绝，**数据一律走共享上下文**；
- 被等待的 promise 被拒绝 → 协程以该拒绝码终止（透传，与 then 的失败即停一致）。

```text
promise ：一层做完做下一层（then 失败即停）
协程    ：一个 await 做完做下一个 await（被拒绝即终止）
```

## 2. JS / C# 对照

| JS / C# | 本框架 |
| --- | --- |
| `await promise` | `CO_AWAIT(promise)` |
| `await Promise.all([a, b])` | `CO_AWAIT_ALL(a, b)` |
| `return;` | `CO_RETURN_VOID();` / `CO_END();` |
| `return result;` | `CO_RETURN(CPromiseResult::Reject(码));` |
| 局部变量跨 await | 必须写成派生类成员（无栈约束） |

## 3. 快速上手

```cpp
#include "Coroutine/Coroutine.h"

struct CMyContext
{
    std::string strData;
    int nCount;
};

/// 处理器：签名固定（上一层结果 + 共享上下文）。
common::async::CPromiseResult StepLoad(common::async::CPromiseResult upResult, const std::shared_ptr<CMyContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    spCtx->strData = LoadFromDisk();
    return common::async::CPromiseResult::Resolve();
}

common::async::CPromiseResult StepSave(common::async::CPromiseResult upResult, const std::shared_ptr<CMyContext>& spCtx)
{
    if (upResult.IsRejected())
    {
        return upResult;
    }
    spCtx->nCount += 1;
    return common::async::CPromiseResult::Resolve();
}

/// 协程：顺序代码写异步流程（与子 promise 共享上下文）。
class CMyCoroutine : public common::async::CCoroutine<CMyContext>
{
   public:
    using common::async::CCoroutine<CMyContext>::CCoroutine;  // 继承上下文构造

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(StepLoad));  // 起一条子 promise 并等待（被拒绝则终止）
        CO_AWAIT(NewPromise(StepSave));
        CO_RETURN_VOID();  // 正常结束（兑现）
        CO_END();          // 兜底：正常结束
    }
};

// 启动
common::async::CAsyncExecutor exec(2);
exec.Start();
std::shared_ptr<CMyContext> spCtx = std::make_shared<CMyContext>();
std::shared_ptr<CMyCoroutine> pCoro = exec.CoStart<CMyCoroutine>(spCtx);

common::async::CPromiseResult r = pCoro->Await();  // 阻塞取最终结果（不抛异常）
if (r.IsFulfilled())
{
    Use(spCtx->strData);  // 数据从共享上下文取
}
```

> `CoStart` 返回的 `shared_ptr` 须持有到完成；框架内部 Resume / 回调持自持强引用，
> 提前释放也不会悬垂（对象存活到最后一个 Resume 执行完）。

## 4. 宏参考

| 宏 | 语义 |
| --- | --- |
| `CO_BEGIN()` | 协程体开始（展开 Duff's device 的 switch 骨架） |
| `CO_AWAIT(expr)` | 等待一条 promise（expr 须可绑定到 `const CPromise<TContext>&`） |
| `CO_AWAIT_ALL(a, b, ...)` | 并行等待多条 promise，全部 settled 后恢复 |
| `CO_RETURN(result)` | 以指定结果结束协程（可兑现可拒绝） |
| `CO_RETURN_VOID()` | 正常结束（兑现） |
| `CO_END()` | 协程体收尾（兜底，正常结束） |

规则：

- 每个宏**独占一行**（`__LINE__` 作恢复点标签，同一行两个宏会冲突）；
- `CO_BEGIN` / `CO_END` 必须保留（状态机的 switch 骨架）；
- 协程体内不能使用「会在恢复点之后跳过」的函数局部变量声明（见第 8 节）。

## 5. 数据传递（共享上下文）

await **不传递数据**，只表示「等到了 / 被拒绝了」。数据走上下文：

```cpp
void Run() override
{
    CO_BEGIN();
    CO_AWAIT(NewPromise(StepLoad));    // 子 promise 把数据写进 GetContext()
    GetContext()->strData += "-done";  // 恢复后直接读写（同一实例）
    CO_AWAIT(NewPromise(StepSave));
    CO_RETURN_VOID();
    CO_END();
}
```

- `NewPromise(处理器)` 起的子 promise 与协程**共用同一执行器与同一上下文**；
- 外部注入上下文：`exec.CoStart<CMyCoroutine>(spCtx)`，协程与所有子 promise 都用 `spCtx`；
- 跨 await 保存的普通值（非上下文里的字段）必须写成派生类成员：

```cpp
class CRetryCoroutine : public common::async::CCoroutine<CMyContext>
{
   public:
    explicit CRetryCoroutine(const std::shared_ptr<CMyContext>& spCtx, int nRetry)
        : common::async::CCoroutine<CMyContext>(spCtx), m_nRetry(nRetry)
    {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(StepLoad));
        --m_nRetry;  // 成员变量：可安全跨 await
        CO_AWAIT(NewPromise(StepSave));
        CO_RETURN_VOID();
        CO_END();
    }

   private:
    int m_nRetry;  // 跨 await 的状态必须放成员
};
```

## 6. 子 promise 与嵌套协程

```cpp
// 子 promise（复用协程的执行器与上下文）
CO_AWAIT(NewPromise(StepLoad));
CO_AWAIT(NewPromise(StepLoad).Then(StepSave));  // 多步子 promise

// 跨上下文：await 另一套 TContext 的子流程（跨流程 / 跨模块组合）
m_spSub = std::make_shared<CSubContext>();  // 跨 await → 成员变量
CO_AWAIT(m_pExec->NewPromise(m_spSub, &StepQueryRows, ASYNC_LOC));

// 并行 await（列表里可以混合不同上下文类型的 promise）
CO_AWAIT_ALL(NewPromise(&StepA), m_pExec->NewPromise(m_spSubA, &StepQueryRows, ASYNC_LOC),
             m_pExec->NewPromise(m_spSubB, &StepQueryRows, ASYNC_LOC));

// 子协程（先启动，再把它的完成状态当 promise await）
m_pChild = m_pExec->CoStart<CChildCoro>(GetContext());  // 跨 await → 成员变量
CO_AWAIT(m_pChild->AsPromise());
```

`AsPromise()` 把协程的完成状态暴露成 promise 句柄，因此：

- 可以在**协程内** await 子协程；
- 也可以在**协程外**注册 settled 通知：

```cpp
pCoro->AsPromise().OnSettled([](common::async::CPromiseResult r) { /* 协程跑完 */ });
```

> 嵌套能力总览（含层内嵌套、阻塞风险、并发写上下文注意点）见
> [async-usage.md 第 6 节](async-usage.md#6-嵌套用法异步里再起异步)。

## 7. 终止语义

- await 的 promise 被拒绝 → 协程**终止**，拒绝码透传；后续 await 不再执行；
- `CO_RETURN(CPromiseResult::Reject(码))` → 主动以拒绝结束；
- `CO_RETURN_VOID()` / `CO_END()` → 正常结束（兑现）；
- 执行器未启动 / 已停止 → 协程立即以 `kStopped` 结束（`Await()` 不阻塞）。

```cpp
common::async::CPromiseResult r = pCoro->Await();
if (r.IsRejected())
{
    // r.Code() 即拒绝码（业务码 / kStopped / kException）
}
```

`CO_AWAIT_ALL` 中任一条被拒绝 → 协程以**首个拒绝码**终止（仍等全部结束，避免对象提前释放）。

## 8. 生命周期与限制

| 情况 | 行为 |
| --- | --- |
| 持有 `CoStart` 返回值 | 正常：`Await()` 取结果 |
| 提前释放 `shared_ptr` | 安全：Resume / 回调捕获自持强引用，对象存活到最后一个 Resume 完成 |
| 执行器 `Stop()` | 已挂起的 await 以 `kStopped` 结束（不悬垂、不阻塞） |
| 同一对象再次 `Start(&exec)` | 复位后重新执行（`Await()` 取新结果） |

限制（无栈协程固有）：

1. **跨 await 的变量必须是成员**（函数局部变量会在恢复点被跳过 / 生命周期错乱）；
2. `CO_BEGIN` / `CO_END` 必须保留，宏不得跨函数；
3. 每个宏独占一行；
4. await 次数多的协程，源码行数随之增加（宏按行展开，不能循环）；
5. 协程体是状态机，**不能用 `return` 提前退出**（除宏自身），需要提前结束用 `CO_RETURN(...)`。

## 9. 与 promise 的关系

| 维度 | promise `CPromise` | 协程 `CCoroutine` |
| --- | --- | --- |
| 单位 | 一层（then / catch / finally） | 一个 await |
| 失败 | then 失败即停（catch 可回滚 / 恢复） | 终止（透传拒绝码） |
| 数据 | 共享上下文 | 共享上下文（同一实例） |
| 可等待 | 是（`OnSettled`） | 是（`AsPromise()`） |
| 适合 | 线性业务流程 / 步骤编排 | 多段异步步骤、顺序读写同一份数据 |

选择建议：**层内是同步逻辑 → 用 promise；需要在多个异步步骤之间保持顺序与局部状态 → 用协程。**

并行汇聚有两条路：promise 侧用执行器上的 `exec.WhenAll` / `WhenAllSettled` / `WhenRace` / `WhenAny`
（聚合链，见 [async-usage.md §10](async-usage.md)）；协程侧用 `CO_AWAIT_ALL`（等全部落定、
首个拒绝码终止协程）。多步骤且需局部状态时用协程；只需等一群分支收口时用组合器。

## 10. 测试与示例

- 单元测试：`Tests/test_async_chain.cpp` 的 `Coro_*` 共 10 个用例
  （顺序 / 并行 / await 拒绝 / 主动拒绝 / 嵌套 / 跨上下文嵌套 / 未启动 / 重启 /
  settled 通知 / 跨上下文并行 await）；
- 示例：`examples/main.cpp` 的 ⑯–⑲；业务侧见 `ServerExample/Module/ExampleAsyncModule.cpp`；
- 基准：`Benchmark/cases/CoroutineCase.cpp`、`ResumableCase.cpp`。
