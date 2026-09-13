# 文档总览

本项目文档统一存放在根目录 `docs/`，按 **ServerCore 组件** 与 **Common 基础库** 分目录，
每个组件/库通常有两份文档：

- **`*-usage.md`**：使用文档（怎么用：API、示例、约束）
- **`*-impl.md`**：实现文档（怎么实现：数据结构、算法、线程模型）

## 总体

| 文档 | 内容 |
|---|---|
| [architecture.md](architecture.md) | 总体架构与分层设计 |

## ServerCore 组件（`docs/servercore/`）

| 组件 | 使用 | 实现 |
|---|---|---|
| 模块系统 | [module-system-usage.md](servercore/module-system-usage.md) | [module-system-impl.md](servercore/module-system-impl.md) |
| 依赖注入 | [dependency-injection-usage.md](servercore/dependency-injection-usage.md) | [dependency-injection-impl.md](servercore/dependency-injection-impl.md) |
| 事件系统 | [events-usage.md](servercore/events-usage.md) | [events-impl.md](servercore/events-impl.md) |
| 消息流水线 | [messaging-usage.md](servercore/messaging-usage.md) | [messaging-impl.md](servercore/messaging-impl.md) |
| 网络层 | [network-usage.md](servercore/network-usage.md) | [network-impl.md](servercore/network-impl.md) |
| 可观测性 | [observability-usage.md](servercore/observability-usage.md) | [observability-impl.md](servercore/observability-impl.md) |
| 并发调度（Exec） | [exec-usage.md](servercore/exec-usage.md) | [exec-impl.md](servercore/exec-impl.md) |
| 扩展指南 | [extensibility-usage.md](servercore/extensibility-usage.md) | [extensibility-impl.md](servercore/extensibility-impl.md) |
| 测试方法 | [testing-usage.md](servercore/testing-usage.md) | [testing-impl.md](servercore/testing-impl.md) |

## Common 基础库（`docs/common/`）

| 组件 | 使用 | 实现 |
|---|---|---|
| 序列化 | [serialization-usage.md](common/serialization-usage.md) | [serialization-impl.md](common/serialization-impl.md) |
| 异步 promise（CAsyncExecutor / CPromise） | [async-usage.md](common/async-usage.md) | [async-impl.md](common/async-impl.md) |
| 协程库（CCoroutine，`Common/Coroutine`） | [coroutine-usage.md](common/coroutine-usage.md) | [coroutine-impl.md](common/coroutine-impl.md) |
| 组合器（`WhenAll` / `AllSettled` / `Race` / `Any`） | [async-usage.md §10](common/async-usage.md) | [async-impl.md](common/async-impl.md) |
| 契约断言（`ASSERT` / `ASSERT_MSG`，`Common/Assert.h`） | [assert-usage.md](common/assert-usage.md) | —（头文件即实现） |

### 示例（`docs/common/`）

| 文档 | 内容 |
|---|---|
| [async-mixed-then-example.md](common/async-mixed-then-example.md) | 单文件示例：一条链里混用多种 then（具名函数 / lambda / 跨模块异步 / 内层链 / 旁支 / catch / finally） |
| [async-vs-js.md](common/async-vs-js.md) | 与 JavaScript Promise 的对照：**只对齐链语义**（调度在 JS 里无对应物，见该文 §0）、语义差异、从 JS 迁过来容易踩的坑 |

### 写法对照（`docs/common/`）

**同一个业务流**的几种写法摆在一篇里对照 —— 用例是一次真实的跨模块下单（下单服务 → 库存模块预占 →
支付模块扣款，失败要补偿）；抽出来就是一个可编译可运行的程序，文档里是实测输出：

| 文档 | 内容 |
|---|---|
| [async-style.md](common/async-style.md) | 五种写法：then 链 + `ThenBridge`（默认）/ 失败补偿（`Catch` 分流 + 反向桥接）/ 协程（`CO_AWAIT` 跨模块）/ 手写桥接（不用 `ThenBridge`）/ 并行调用（`WhenAll`）；含公共部分、选型对照表与判据 |

### 问题记录（`docs/common/`）

| 文档 | 内容 |
|---|---|
| [async-cross-module-findings.md](common/async-cross-module-findings.md) | 跨模块异步的两个真问题（续跑线程二选一、`OnSettled` 返回值漏检导致挂死）：现象 / 复现 / 根因 / 规避 —— **两个问题均已在框架层修复**（见正文「已修复」标记） |

## 其他

| 文档 | 内容 |
|---|---|
| [perf-optimization.md](perf-optimization.md) | 性能优化记录 |
| [vscode-select-dropdown.md](vscode-select-dropdown.md) | VS Code 选择下拉（开发环境备忘） |
| [vscode-tasks-launch.md](vscode-tasks-launch.md) | VS Code tasks/launch 运行逻辑与字段详解 |
| [vscode-clangd-format.md](vscode-clangd-format.md) | clangd 格式化 / 缩进约定（.clang-format、.clangd、settings.json；排错与排版建议） |

## 阅读建议

- **新成员上手**：`architecture.md` → 模块系统 → 依赖注入 → 网络 / 消息 / 事件 → 用 ServerExample 跑通
- **写业务模块**：`extensibility-usage.md`（新增模块/协议/服务器）+ `dependency-injection-usage.md`
- **并发控制**：`servercore/exec-usage.md`（模块读写调度 + 业务流程回调栈）
- **异步/协程**：`common/async-usage.md`、`common/coroutine-usage.md`
- **深入实现**：任一组件的 `*-impl.md`（数据结构、算法、线程模型）

> 历史说明：旧文档位于各项目的 `docs/` 目录，已统一迁移/拆分至本目录（`ServerCore/docs`、`Common/Async/docs` 已移除）。
