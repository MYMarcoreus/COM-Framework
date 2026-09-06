# DataHub 应用总览（数据面 + 控制面）

> 本文是 **DataHub 应用工程文档集**的入口页。它回答“DataHub 是什么、由哪几部分组成、
> 怎么跑起来、文档怎么导航”。实现细节见各分篇（见文末“文档导航”）。
>
> 应用分组：**DataHub**（数据传输服务/数据面）与 **DataHubAdmin**（独立控制面 exe）。

## 1. 这是什么

**DataHub** 是一个基于 **ServerCore 骨架 + Sogou Workflow** 的局域网“聊天室”数据传输服务：
手机、电脑等设备接入同一网络后，用浏览器即可像聊天一样实时互传**文本与文件**。

随着演进，DataHub 从“单房间聊天”成长为**带租户边界（组织）的多租户应用**，并拆分出独立的
**服务端管理控制面**：

```text
┌─────────────────────────────┐        ┌──────────────────────────────┐
│  DataHub  (数据面, exe)      │        │  DataHubAdmin (控制面, exe)   │
│  监听 0.0.0.0:8888           │        │  监听回环 127.0.0.1:8899      │
│  - 前端：/ 选择页 + /chat 聊天 │        │  - 前端：管理网页             │
│  - 业务/租户/设备 HTTP API     │        │  - /api/admin/* 仅代理转发    │
│  - 内存存储（租户隔离）        │        │    （回环 + X-Admin-Token）    │
└───────────▲─────────────────┘        └───────────▲──────────────────┘
            │ 局域网设备浏览器访问                     │ 仅本机浏览器访问
            │ X-Tenant / X-Client-Id / X-Token        │ 令牌只在服务器侧注入
```

- **数据面**（DataHub：`datahub` 可执行文件）：为设备提供聊天/文件读写，真正持有数据；
  多租户数据、租户花名册、在线成员、设备凭据都在此进程内（纯内存）。
- **控制面**（DataHubAdmin：`datahub-admin` 可执行文件）：**不持有任何业务数据**，只提供
  管理网页，并把 `/api/admin/*` 原样**代理**到数据面本机回环 + 注入 `X-Admin-Token`。
  因此管理令牌永不暴露给浏览器，管理操作仅限本机回环。

> 为什么是两个独立 exe 而不是一个模块？——控制面/数据面**进程级分离**：数据面监听外网供
> 局域网设备访问，攻击面大；控制面只绑回环、只在本机可用，即使数据面被攻破，管理令牌与
> 回环限制仍在控制面侧独立成环。详见 `datahub-architecture.md`。

## 2. 核心能力

| 能力 | 说明 | 文档 |
|---|---|---|
| 文本/文件/图片互传 | `POST /api/text`、`POST /api/file`；图片内联预览，文件附件下载 | `datahub-http-api.md` |
| 多租户（组织） | 公共租户 `public` + 私有租户（6 位分享码）；Owner/Member 角色 | `tenant-project.md` |
| 设备账号 | 每设备注册随机令牌：`X-Client-Id` + `X-Token` 双因子式校验，防身份伪造 | `datahub-http-api.md`、`tenant-audit.md`(B1) |
| 数据隔离 | 共享表 + 行级 `tenant` 边界；业务 API 须显式 `X-Tenant` | `datahub-storage.md` |
| 在线成员 | 基于 `X-Client-Id` 的在线活跃列表（30s TTL） | `datahub-http-api.md`、`datahub-frontend.md` |
| 增量同步 | `GET /api/list?since=<seq>` 游标拉新，不随历史增长 | `datahub-http-api.md` |
| 断线自愈 | 重启后令牌幂等重注册 + 状态对账回落（`/api/tenant/state`） | `datahub-frontend.md`、`tenant-audit.md`(B2/B6) |
| 管理控制面 | DataHubAdmin 代理运维：租户/成员/条目/配额/审计 | `datahub-http-api.md`（管理段） |

## 3. 目录导览（主工程 `DataHub/`）

```text
DataHub/
├── main.cpp                       # 入口：端口参数、SIGPIPE
├── run.sh                         # 一键：编译 + WSL 端口转发 + 启动
├── datahub.ini                    # 数据面配置（[server][web][log][http][admin][store]）
├── Application/
│   └── DataHubApplication.*       # CDataHubApplication：装配 Tenant/Store/Http 模块
├── Framework/                     # 通用 Web 框架层 web::（只依赖 workflow，可移植）
│   ├── HttpMessage.*              #   CHttpRequest / CHttpResponse（含 Range 206）
│   ├── HttpRouter.*               #   CHttpRouter 路径模板路由
│   ├── HttpText.*                 #   编解码 / MIME / 文件读取（纯函数）
│   └── README.md
├── Module/
│   ├── Tenant/                    # ITenantService + CTenantModule（租户/花名册/角色）
│   ├── Storage/                   # IDataStore + DataStoreModule（封装 Common CFileStore）
│   ├── Http/                      # HttpServerModule + 各控制器/服务（业务、租户、设备、页面）
│   └── Admin/                     # CAdminController（/api/admin/*，本机回环 + 令牌）
├── Web/                           # 前端源文件（部署到 ~/.datahub）
│   ├── tenants.html / tenants.js  #   /       租户选择/管理页
│   ├── index.html / app.js        #   /chat   聊天室页
│   ├── common.js                  #   共享：CLIENT_ID、租户状态、设备令牌、apiFetch
│   └── style.css
└── Linux/Makefile                 # 产物 build/<mode>/datahub + 部署 ~/.datahub
```

### 姊妹工程 `DataHubAdmin/`

```text
DataHubAdmin/
├── main.cpp                       # 入口
├── datahub-admin.ini              # 控制面配置（[server][upstream][web][log]）
├── Application/                   # CDataHubAdminApplication：装配 CAdminConsoleModule
├── Module/
│   └── AdminConsoleModule.*       # 回环校验 + 页面 + /api/admin/* 代理到数据面
├── Web/
│   ├── index.html / admin.js      # 管理网页（部署到 ~/.datahub-admin）
└── Linux/Makefile                 # 产物 build/<mode>/datahub-admin + 部署
```

## 4. 快速上手

```bash
# 构建（debug；自动把 Web/* 部署到 ~/.datahub，DataHubAdmin 的到 ~/.datahub-admin）
./build.sh --debug DataHub DataHubAdmin

# 运行数据面 —— 注意：须在含 datahub.ini 的 DataHub/ 目录启动（配置按 cwd 读取）
cd DataHub && ../build/debug/datahub            # 默认读 datahub.ini 的 8888
# 运行控制面
cd DataHubAdmin && ../build/debug/datahub-admin

# 访问
#   http://<服务器IP>:8888/   （数据面：设备浏览器，选择/进入租户后聊天）
#   http://127.0.0.1:8899/    （控制面：仅本机，租户/成员/数据管理）
```

> WSL2（NAT）下局域网设备需先做 Windows 端口转发，`DataHub/run.sh` 自动处理。

## 5. 概念提醒：别混淆这三套“成员/账号”

DataHub 里同名相近、职责不同的三套概念（详见 `datahub-storage.md`）：

1. **租户花名册**（`CTenantModule`，`/api/tenant/members`）：加入某租户的正式成员，
   带 Owner/Member 角色，决定删除授权与角色展示；内存持久于模块内。
2. **在线活跃**（`CMemberService`，`/api/members`）：30s TTL 的“此刻在线”列表，
   仅记录携带 `X-Client-Id` 的请求，按租户区分。
3. **设备凭据**（`CDeviceRegistry`，`/api/device/register`）：身份认证层，
   证明“我确实是该 clientId 的设备”，不等于租户成员。

## 6. 文档导航

### DataHub 应用工程文档集（本文所在组）

| 文档 | 内容 |
|---|---|
| [datahub-overview.md](datahub-overview.md) | 本页：是什么 / 双服务器 / 目录 / 快速上手 / 导航 |
| [datahub-architecture.md](datahub-architecture.md) | 分层与模块装配、请求生命周期、线程模型、扩展点 |
| [datahub-http-api.md](datahub-http-api.md) | 完整 HTTP API 参考：请求头、端点、错误码、curl 示例 |
| [datahub-storage.md](datahub-storage.md) | 三套内存模型、数据隔离、配额、删除级联、重启语义 |
| [datahub-frontend.md](datahub-frontend.md) | 前端两页 + 控制面 UI：令牌注册、轮询、对账自愈 |
| [datahub-configuration.md](datahub-configuration.md) | ini 键表、构建/部署、运行、日志审计、排障 |

### 租户主题专题（既有，横切视角）

| 文档 | 内容 |
|---|---|
| [tenant-concepts.md](tenant-concepts.md) | 租户/多租户通用概念（先单租户后多租户） |
| [tenant-industry.md](tenant-industry.md) | 业界实现（产品/技术栈/控制面数据面/实时推送） |
| [tenant-project.md](tenant-project.md) | 租户能力在本项目的落地（实体/模型/闸门/双服务器） |
| [tenant-audit.md](tenant-audit.md) | 安全审计 B1–B6 与修复（身份/离场/审计/显式租户/限流/重启） |

> 阅读路径建议：本页 → `datahub-architecture.md`（看懂代码）→ `datahub-http-api.md`（上手调用）→
> 需要做租户改造时再读 tenant-* 专题。
