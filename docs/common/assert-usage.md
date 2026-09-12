# 契约断言 ASSERT —— 使用文档

> 头文件：`Common/Assert.h`（全框架通用，Common / ServerCore / 业务模块 / 测试 / 示例都用它）
> 规范出处：[.github/skills/cpp-development/SKILL.md](../../.github/skills/cpp-development/SKILL.md) §6.1

## 1. 它解决什么问题

「调用前提」和「内部不变量」如果不写成断言，就只有两条路：

- 写成运行时的宽容分支（静默返回、空操作、特例检查）—— 分支长期留在热路径上，
  而且让误用拖到很晚才暴露（典型：链悄悄不跑、句柄悄悄失效）；
- 什么都不写 —— 崩在别的地方，定位成本极高。

断言把这两类问题**在开发期当场钉住**，并且发布构建零开销。

## 2. 用法

```cpp
#include "Assert.h"

ASSERT(pCore != nullptr);                                        // 内部不变量
ASSERT_MSG(spContext != nullptr, "共享上下文必须由调用方传入");  // 带说明
```

失败时打印「表达式 / 位置 / 函数」后 `abort()`：

```text
[ASSERT 失败] spContext != nullptr —— 共享上下文必须由调用方传入
  位置: /path/to/Promise.h:513
  函数: common::async::detail::CPromiseCore<TContext>::CPromiseCore(...)
```

## 3. 开关（`FRAMEWORK_DEBUG`）

```cpp
// Common/Assert.h
#if !defined(NDEBUG) && !defined(__OPTIMIZE__)
    #define FRAMEWORK_DEBUG 1
#else
    #define FRAMEWORK_DEBUG 0
#endif
```

| 构建 | NDEBUG | 优化 | `FRAMEWORK_DEBUG` | ASSERT |
| --- | --- | --- | --- | --- |
| debug（`./build.sh --debug`） | 未定义 | `-O0` | 1 | 生效（失败即 abort） |
| release（`./build.sh --release`） | **定义**（`-DNDEBUG`） | `-O2` | 0 | 展开为 `(void)sizeof(expr)`：**不求值、零开销、不引入分支** |

两个条件同时要求是有意的：既符合 C++ 标准语义（`NDEBUG` 关闭断言），又能在
「发布构建忘记加 `-DNDEBUG`」时兜底（`-O2` 一定关闭断言，绝不会把 `abort()` 带上线）。

`FRAMEWORK_DEBUG` 是**全框架唯一的调试判定**：`Common/Async/SourceLoc.h`（`ASYNC_LOC`
注册点）与 `Common/Async/Diagnostics.cpp`（诊断默认打印）都用它，不要在各处另写一套。

## 4. 什么时候该用 / 不该用

| 场景 | 用什么 |
| --- | --- |
| 内部不变量（状态指针非空、载荷存在） | `ASSERT` |
| 调用前提（上下文必传、模块已启动、必须在 `CoStart` 之后 `await`） | `ASSERT` / `ASSERT_MSG` |
| 明显写错的入参（`Reject(0)`：0 是兑现码） | `ASSERT_MSG` |
| 业务错误（参数非法、查不到、权限不足…） | **返回值 / 错误码 / 异常 / 日志**（异步里是 `CPromiseResult` 的拒绝码） |
| 正常分支（投递失败、对象已停止、子流程被拒绝） | **普通分支**，不是断言 |

一句话判断：**「不满足就是编程错误」→ 断言；「不满足也算正常」→ 分支。**

## 5. 当前使用点

| 位置 | 断言 |
| --- | --- |
| `Common/Async/Promise.h`：`CPromiseCore` 构造 | 共享上下文非空 |
| `Common/Async/Promise.h`：`CPromise` 私有构造 | 共享核心非空（无「无效句柄」态） |
| `Common/Async/Promise.h`：`MakeHandlerRunner` / `RunHandler` / `PostHandler` | 上下文 / 层状态非空 |
| `Common/Async/PromiseResult.h`：`Reject` | 拒绝码不是 0（0 是兑现码） |
| `Common/Coroutine/Coroutine.h`：构造 / `Await` / `AsPromise` / `AwaitWait` / `AwaitEach` | 上下文非空；必须在 `CoStart` 之后 |
| `ServerExample/Module/Example{Db,Async}Module.cpp` | 模块已启动、入参非空（业务侧示范） |
| `examples/main.cpp` | 示例自校验（与框架同一套断言） |

## 6. 相关取舍

- **为什么不用 `<cassert>`**：本项目要打印函数名与自定义说明，且调试判定要与 `ASYNC_LOC`
  统一（不能只依赖 `NDEBUG`，见 §3）。
- **为什么不用「返回值 + 兜底」替代断言**：那是把编程错误伪装成可恢复错误；
  但如果调用方**确实**能处理（例如执行器已停 → 链以 `kStopped` 收口），就必须走拒绝码，
  不能断言。
- **断言不能代替测试**：断言证明「不变量没被破坏」，不证明「行为正确」——
  行为契约仍然靠 `Tests/` 里的用例覆盖（异步用例见 [async-impl.md](async-impl.md) §11）。
