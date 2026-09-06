# 租户（Tenant）业界与其他项目实现

> **本文档讲业界/其它项目是怎么实现多租户的**，供对照学习。
> 相关文档：[租户概念详解](tenant-concepts.md)（通用地基） · [本项目（DataHub）实现](tenant-project.md)

## 1. 真实产品怎么做

| 产品 | 它的"租户" | 存储/隔离选择 | 可借鉴点 |
|---|---|---|---|
| **Salesforce** | Org | 早期就多租户共享数据库 + **元数据驱动**（租户 id 进主键） | 单 schema 海量租户可行 |
| **Slack** | Workspace | workspace + channel 两级；**membership 驱动**加入 | "先进 workspace 再协作"交互心智 |
| **GitHub** | Organization | org→repo→team 多层；角色 owner/member | 组织模型与权限分层 |
| **Atlassian/Jira** | Site | site=租户（独立子域/命名空间） | 站点隔离 + 白标域名 |
| **飞书/企微/钉钉** | 企业/组织 | 企业=租户，内部再分部门/群 | 组织树、成员管理完善 |
| **Notion/语雀** | Space/团队 | space=租户、页面在 space 内 | "文档空间"式协作隔离 |
| **AWS** | Account | 账号=强隔离边界 + 成本/权限单位 | 隔离与计费都绑账号 |
| **Azure/Entra** | Tenant（目录） | 目录级身份隔离 | 身份/目录即租户 |
| **K8s** | Namespace | 软隔离，配 RBAC/网络策略 | namespace 心智可类比 |

> 启示：成熟产品几乎都是"**租户入口先行**（选/建组织）+ **组织内共享** + **成员/角色管理** + **数据与实时都按组织隔离**"。

## 2. 主流技术栈怎么落地

- **Spring（Java）**：`TenantContext`（ThreadLocal）由拦截器从 header/子域填好 → 数据访问用 **Hibernate 过滤器或 MyBatis 拦截器自动拼 tenant 条件**；租户多库用动态数据源 / `apartment`（每 schema 一 context）。
- **Ruby/Rails**：`apartment`（schema-per-tenant）或 `acts_as_tenant`（shared table + 自动 scope）。
- **Django**：`django-tenants`（schema）；`django-multitenant`（shared）。
- **Node/Go**：靠**请求级 context/中间件**（`context.WithValue(ctx, tenantID)`）贯穿，数据层强制拼接；Go 常用 gorm 回调或 wrapper 统一 `Where("tenant_id=?")`。
- **云数据库**：Postgres **行级安全（RLS）** 做共享表双保险；Snowflake/云数仓按组织命名空间。

> 语言无关的通用三件套 = **① 请求级租户上下文 ② 数据层统一注入/RLS ③ 跨层租户键（缓存/文件/任务/日志）**。

## 3. 账号、身份与多组织

- **统一身份认证（IdP）**一次登录 → 得到"我的账号 + 我属于哪些组织 + 各组织的角色"；
- **选组织进入**（入口常是组织选择器，如 Slack 登录后先列 workspace）；
- 组织内再授权（RBAC/ABAC），成员/邀请/离职同步。

## 4. 控制面（Admin）与数据面分离

中大型系统把"租户运营"独立成控制面：

- **数据面**跑业务流量（按租户隔离的数据、实时协作）；
- **控制面**管租户生命周期（开通/停用/配额/计费）、成员与角色、审计、全局观测；
- 两者可同一进程（小型）、独立服务、甚至独立账号体系（运营侧 RBAC）。

## 5. 实时与协作：租户边界不止在数据库

- **在线成员（presence）按租户/会话隔离**，不跨租户看到谁在线；
- **推送按租户订阅**：事件只发到"该租户内成员的连接"（WebSocket/SSE 频道 = `tenant:{id}`）；
- 被踢/离开 = 服务端**主动关连接/撤订阅**（在线由连接生命周期决定，而非心跳猜）；
- 会话重连后**对账**（拉租户名/角色/是否仍存在），补齐离线漏掉的事件。

## 6. 业界共识速记

1. 租户 id 用不可变标识，数据只引用它、不按名字快照；
2. 查询强制租户谓词 + 行级安全双保险；
3. 跨层（缓存/文件/队列/日志）统一带租户键；
4. 账号 ≠ 租户：membership + role 第三层建模；
5. 交互先"选/建组织"，组织内协作；
6. 被踢/解散要给明确事件与离场，不是悄悄 404；
7. 控制面与数据面分离，租户可运营。

> 下一站：[本项目（DataHub）实现](tenant-project.md)——看这些共识如何被落地。
