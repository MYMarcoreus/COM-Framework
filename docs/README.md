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

按**两条线**组织：应用工程文档（面向工程本身）与租户主题专题（横切视角，先概念后落地）。

### 应用工程文档集

| 文档 | 内容 |
|---|---|
| [datahub-overview.md](datahub/datahub-overview.md) | 应用总览入口：是什么/数据面+控制面双服务器/目录/快速上手/文档导航 |
| [datahub-architecture.md](datahub/datahub-architecture.md) | 架构：分层/模块装配/请求生命周期(OnRequest)/线程模型/控制面代理/扩展点 |
| [datahub-http-api.md](datahub/datahub-http-api.md) | 完整 HTTP API 参考：请求头(X-Tenant/X-Token…)/设备/租户/业务/管理端点/错误码/curl |
| [datahub-storage.md](datahub/datahub-storage.md) | 存储与状态：三套内存模型(花名册/数据项/在线)/隔离/配额/删除级联/重启语义 |
| [datahub-frontend.md](datahub/datahub-frontend.md) | 前端：两页(选择/聊天)+控制面 UI/共享状态与设备令牌/轮询与对账自愈 |
| [datahub-configuration.md](datahub/datahub-configuration.md) | 配置/构建/运行/排障：ini 键表/部署目录/日志审计/指标/常见问题 |

### 租户主题专题

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

- **DataHub（应用）**：`datahub/datahub-overview.md` → `datahub-architecture.md` → `datahub-http-api.md`
  （改多租户逻辑再读 `tenant-project.md` / `tenant-audit.md`）
- **新成员上手**：`architecture.md` → 模块系统 → 依赖注入 → 网络 / 消息 / 事件 → 用 ServerExample 跑通
- **写业务模块**：`extensibility-usage.md`（新增模块/协议/服务器）+ `dependency-injection-usage.md`
- **并发控制**：`servercore/exec-usage.md`（模块读写调度 + 业务流程回调栈）
- **异步/协程**：`common/async-usage.md`、`common/coroutine-usage.md`
- **深入实现**：任一组件的 `*-impl.md`（数据结构、算法、线程模型）

> 历史说明：旧文档位于各项目的 `docs/` 目录，已统一迁移/拆分至本目录（`ServerCore/docs`、`Common/Async/docs` 已移除）。
