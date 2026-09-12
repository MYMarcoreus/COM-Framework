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

### 示例（`docs/common/`）

| 文档 | 内容 |
|---|---|
| [async-mixed-then-example.md](common/async-mixed-then-example.md) | 单文件示例：一条链里混用多种 then（具名函数 / lambda / 跨模块异步 / 内层链 / 旁支 / catch / finally） |
| [async-vs-js.md](common/async-vs-js.md) | 与 JavaScript Promise 的对照：API 对应、语义差异、从 JS 迁过来容易踩的坑 |

### 风格模仿（`docs/common/`）

把其他异步框架 / 语言的写法，用本框架临摹一遍（每篇一个可编译运行的用例）：

| 文档 | 对标写法 |
|---|---|
| [async-style-coroutine.md](common/async-style-coroutine.md) | libgo：一个协程里直线书写，业务拒绝中断，统一兜底 |
| [async-style-then-chain.md](common/async-style-then-chain.md) | JavaScript Promise：then 链 + 内层链 + reject + catch |
| [async-style-manual-settle.md](common/async-style-manual-settle.md) | async_promise：`make_promise(resolve, reject)` 显式兑现 / 拒绝 |
| [async-style-recover.md](common/async-style-recover.md) | Async++：手动完成 + `.recover` 分流兜底后继续链 |

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
