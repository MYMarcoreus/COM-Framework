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
| 异步库（CAsyncExecutor/CTask） | [async-usage.md](common/async-usage.md) | [async-impl.md](common/async-impl.md) |
| 协程库（CCoroutine） | [coroutine-usage.md](common/coroutine-usage.md) | [coroutine-impl.md](common/coroutine-impl.md) |

## DataHub 应用（`docs/datahub/`）

| 文档 | 内容 |
|---|---|
| [tenant-concepts.md](datahub/tenant-concepts.md) | 租户/多租户概念详解（与项目无关）：先讲单租户再讲多租户（动机/隔离维度/三类存储模型/应用层租户上下文/反模式 BOLA/账号-成员-角色） |
| [tenant-industry.md](datahub/tenant-industry.md) | 业界与其他项目实现：Salesforce/Slack/GitHub/AWS/K8s 等产品，Spring/Rails/Django/Node/Go 技术栈，控制面数据面，实时与推送隔离 |
| [tenant-project.md](datahub/tenant-project.md) | 本项目（DataHub）落地：选型映射/实体/分层/共享表模型/请求生命周期/闸门/API/双服务器/前端/同步问题/限制 |
| [tenant-audit.md](datahub/tenant-audit.md) | 安全审计 B1–B6 与修复：身份可伪造/无离场自愈/无审计/静默回落 public/join 可枚举/重启 404 循环 |

## 其他

| 文档 | 内容 |
|---|---|
| [perf-optimization.md](perf-optimization.md) | 性能优化记录 |
| [vscode-select-dropdown.md](vscode-select-dropdown.md) | VS Code 选择下拉（开发环境备忘） |
| [vscode-tasks-launch.md](vscode-tasks-launch.md) | VS Code tasks/launch 运行逻辑与字段详解 |

## 阅读建议

- **新成员上手**：`architecture.md` → 模块系统 → 依赖注入 → 网络 / 消息 / 事件 → 用 ServerExample 跑通
- **写业务模块**：`extensibility-usage.md`（新增模块/协议/服务器）+ `dependency-injection-usage.md`
- **并发控制**：`servercore/exec-usage.md`（模块读写调度 + 业务流程回调栈）
- **异步/协程**：`common/async-usage.md`、`common/coroutine-usage.md`
- **深入实现**：任一组件的 `*-impl.md`（数据结构、算法、线程模型）

> 历史说明：旧文档位于各项目的 `docs/` 目录，已统一迁移/拆分至本目录（`ServerCore/docs`、`Common/Async/docs` 已移除）。
