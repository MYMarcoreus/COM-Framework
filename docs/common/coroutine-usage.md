# 无栈协程 CCoroutine — 使用文档

> 对应目录：`Common/Async/Coroutine.h`（命名空间 `common::async`）
> 实现细节见：[coroutine-impl.md](coroutine-impl.md) ｜ 链见：[async-usage.md](async-usage.md)

## 1. 这是什么

链负责**编排**（层与层串起来，失败即停）；协程负责**顺序化** ——
用顺序代码 await 多条链，替代回调嵌套。

两者共用同一套模型：

- 协程持有一个共享上下文 `std::shared_ptr<TContext>`（与它起的子链同一实例）；
- await 的对象是**链**（包含子协程 `AsChain()` 暴露的链）；
- await 只告知成功 / 失败，**数据一律走共享上下文**；
- 被等待的链失败 → 协程以该失败码终止（透传，与链的失败即停一致）。

```text
链    ：一层做完做下一层（失败即停）
协程  ：一个 await 做完做下一个 await（失败即终止）
```

## 2. C# 对照

| C# | 本框架 |
| --- | --- |
| `await Task` | `CO_AWAIT(chain)` |
| `await Task.WhenAll(a, b, c)` | `CO_AWAIT_ALL(a, b, c)` |
| `return;`（void 方法结束） | `CO_RETURN_VOID();` / `CO_END();` |
| `return result;` | `CO_RETURN(CStepResult::Failed(码));` |
| 局部变量跨 await | 必须写成派生类成员（无栈约束） |

## 3. 快速上手

```cpp
#include "Async/Coroutine.h"

namespace no = common::async;

struct CMyContext
{
    std::string strData;
    int nCount;
};

/// 层：签名固定（上一层结果 + 共享上下文）。
no::CStepResult StepLoad(no::CStepResult upStep, const std::shared_ptr<CMyContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    spCtx->strData = LoadFromDisk();
    return no::CStepResult::Ok();
}

no::CStepResult StepSave(no::CStepResult upStep, const std::shared_ptr<CMyContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    spCtx->nCount += 1;
    return no::CStepResult::Ok();
}

/// 协程：顺序代码写异步流程（与子链共享上下文）。
class CMyCoroutine : public no::CCoroutine<CMyContext>
{
public:
    using no::CCoroutine<CMyContext>::CCoroutine; // 继承上下文构造

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(Chain(&StepLoad));       // 起一条子链并等待（失败则协程终止）
        CO_AWAIT(Chain(&StepSave));
        CO_RETURN_VOID();                 // 正常结束（成功）
        CO_END();                         // 兜底：正常结束
    }
};

// 启动
no::CAsyncExecutor exec(2);
exec.Start();
std::shared_ptr<CMyContext> spCtx = std::make_shared<CMyContext>();
std::shared_ptr<CMyCoroutine> pCoro = exec.CoStart<CMyCoroutine>(spCtx);

no::CStepResult r = pCoro->Get();         // 阻塞取最终成败（不抛异常）
if (r.IsOk())
{
    Use(spCtx->strData);                  // 数据从共享上下文取
}
```

> `CoStart` 返回的 `shared_ptr` 须持有到完成；框架内部 Resume / 回调持自持强引用，
> 提前释放也不会悬垂（对象存活到最后一个 Resume 执行完）。

## 4. 宏参考

| 宏 | 语义 |
| --- | --- |
| `CO_BEGIN()` | 协程体开始（展开 Duff's device 的 switch 骨架） |
| `CO_AWAIT(expr)` | 等待一条链（expression 结果须可绑定到 `const CAsyncChain<TContext>&`） |
| `CO_AWAIT_ALL(a, b, ...)` | 并行等待多条链，全部结束后恢复 |
| `CO_RETURN(stepResult)` | 以指定结果结束协程（可成功可失败） |
| `CO_RETURN_VOID()` | 正常结束（成功） |
| `CO_END()` | 协程体收尾（兜底，正常结束） |

规则：

- 每个宏**独占一行**（`__LINE__` 作恢复点标签，同一行两个宏会冲突）；
- `CO_BEGIN` / `CO_END` 必须保留（状态机的 switch 骨架）；
- 协程体内不能使用会在恢复点之后跳过的**函数局部变量声明**（见第 9 节）。

## 5. 数据传递（共享上下文）

await **不传递数据**，只表示「等到了 / 失败了」。数据走上下文：

```cpp
void Run() override
{
    CO_BEGIN();
    CO_AWAIT(Chain(&StepLoad));                    // 子链把数据写进 GetContext()
    GetContext()->strData += "-done";              // 恢复后直接读写（同一实例）
    CO_AWAIT(Chain(&StepSave));
    CO_RETURN_VOID();
    CO_END();
}
```

- `Chain(层函数)` 起的子链与协程**共用同一执行器与同一上下文**；
- 外部注入上下文：`exec.CoStart<CMyCoroutine>(spCtx)`，协程与所有子链都用 `spCtx`；
- 需要跨 await 保存的**普通值**（非上下文里的字段）必须写成派生类成员：

```cpp
class CRetryCoroutine : public no::CCoroutine<CMyContext>
{
public:
    explicit CRetryCoroutine(const std::shared_ptr<CMyContext>& spCtx, int nRetry)
        : no::CCoroutine<CMyContext>(spCtx), m_nRetry(nRetry) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(Chain(&StepLoad));
        --m_nRetry;                    // 成员变量：可安全跨 await
        CO_AWAIT(Chain(&StepSave));
        CO_RETURN_VOID();
        CO_END();
    }

private:
    int m_nRetry;                      // 跨 await 的状态必须放成员
};
```

## 6. 子链与嵌套协程

```cpp
// 子链（复用协程的执行器与上下文）
CO_AWAIT(Chain(&StepLoad));
CO_AWAIT(Chain(&StepLoad).Then(&StepSave));        // 多步子链

// 子协程（先启动，再把它的完成状态当链 await）
m_pChild = m_pExec->CoStart<CChildCoro>(GetContext());   // 跨 await → 成员变量
CO_AWAIT(m_pChild->AsChain());
```

`AsChain()` 把协程的完成状态暴露成链句柄，因此：

- 可以在**协程内** await 子协程；
- 也可以在**协程外**注册完成回调：

```cpp
pCoro->AsChain().OnCompleted([](no::CStepResult r) { /* 协程跑完 */ });
```

## 7. 终止语义

- await 的链失败 → 协程**终止**，失败码透传；后续 await 不再执行；
- `CO_RETURN(CStepResult::Failed(码))` → 主动以失败结束；
- `CO_RETURN_VOID()` / `CO_END()` → 正常结束（成功）；
- 执行器未启动 / 已停止 → 协程立即以 `kStepStopped` 结束（`Get()` 不阻塞）。

```cpp
no::CStepResult r = pCoro->Get();
if (r.IsFailed())
{
    // r.Code() 即失败码（业务码 / kStepStopped / kStepException）
}
```

`CO_AWAIT_ALL` 中任一条链失败 → 协程以**首个失败码**终止（仍等全部结束，避免对象提前释放）。

## 8. 生命周期

| 情况 | 行为 |
| --- | --- |
| 持有 `CoStart` 返回值 | 正常：`Get()` 取结果 |
| 提前释放 `shared_ptr` | 安全：Resume / 回调捕获自持强引用，对象存活到最后一个 Resume 完成 |
| 执行器 `Stop()` | 已挂起的 await 以 `kStepStopped` 结束（不悬垂、不阻塞） |
| 同一对象再次 `Start(&exec)` | 复位后重新执行（`Get()` 取新结果） |

## 9. 限制（无栈协程固有）

1. **跨 await 的变量必须是成员**（函数局部变量会在恢复点被跳过 / 生命周期错乱）；
2. `CO_BEGIN` / `CO_END` 必须保留，宏不得跨函数；
3. 每个宏独占一行；
4. `await` 次数多的协程，源码行数随之增加（宏按行展开，不能循环）；
5. 协程体是状态机，**不能用 `return` 提前退出**（除宏自身），需要提前结束用
   `CO_RETURN(...)`。

## 10. 与链的关系

| 维度 | 链 `CAsyncChain` | 协程 `CCoroutine` |
| --- | --- | --- |
| 单位 | 一层 | 一个 await |
| 失败 | 失败即停（`ThenAlways` 可回滚） | 终止（透传失败码） |
| 数据 | 共享上下文 | 共享上下文（同一实例） |
| 可等待 | 是（`OnCompleted`） | 是（`AsChain()`） |
| 适合 | 线性业务流程 / 步骤编排 | 需要多段异步步骤、顺序读写同一份数据 |

选择建议：**层内是同步逻辑 → 用链；需要在多个异步步骤之间保持顺序与局部状态 → 用协程。**

## 11. 测试与示例

- 单元测试：`Tests/test_async_chain.cpp` 中 `AsyncChainCoro_*` 共 9 个用例
  （顺序 / 并行 / await 失败 / 主动失败 / 嵌套 / 未启动 / 重启 / 完成回调）；
- 示例：`examples/main.cpp` 的 ⑯–⑲；业务侧见 `ServerExample/Module/ExampleAsyncModule.cpp`；
- 基准：`Benchmark/cases/CoroutineCase.cpp`、`ResumableCase.cpp`。
