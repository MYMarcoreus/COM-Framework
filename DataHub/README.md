# DataHub —— 局域网设备间即时通讯（多租户聊天室 + 独立控制面）

基于 **ServerCore 骨架 + Sogou Workflow** 的局域网聊天室服务：手机、电脑接入同一网络后，
用浏览器即可像聊天一样实时互传**文本与文件**；支持**多租户（组织边界）**，并配有独立的
**服务端管理控制面**（DataHubAdmin）。

> 本 README 是快速上手与导航。**完整文档见 [`docs/datahub/`](../docs/datahub/datahub-overview.md)**
> （总览 / 架构 / HTTP API / 存储 / 前端 / 配置）。

## 双服务器拓扑

```text
DataHub(数据面, exe, :8888)  ← 局域网设备浏览器（X-Tenant/X-Client-Id/X-Token）
DataHubAdmin(控制面, exe, 回环 :8899)  ← 仅本机浏览器；/api/admin/* 代理到数据面
```

- **数据面**：真正持有数据（纯内存、租户隔离），对外提供聊天/文件 API。
- **控制面**：不持业务数据，只做管理网页 + 把 `/api/admin/*` 代理到数据面回环并注入
  `X-Admin-Token`（令牌只在服务器侧）。

## 特性

- **ServerCore 骨架**：`CMyApplication` 生命周期 + 模块模型 + 配置/日志/指标
- **Workflow HTTP**：`WFHttpServer` 高性能异步服务（封装为 `CHttpServerModule`）
- **多租户**：公共租户 `public` + 私有租户（6 位分享码）；Owner/Member 角色；业务 API 显式 `X-Tenant`
- **设备账号**：每设备注册随机令牌（`X-Client-Id` + `X-Token`），防身份伪造
- **纯内存存储**：`common::storage::CFileStore`（文本/文件；重启清空，前端对账自愈）
- **聊天室界面**：IM 风格；消息游标增量轮询（`?since=`）、在线成员、图片内联预览
- **文件/图片**：Range 分段下载（规避 workflow 大响应截断）；中文名 RFC 5987
- **独立控制面**：DataHubAdmin 管理网页（租户/成员/条目/配额/改名/删除，操作留审计日志）
- **独立前端资源**：`Web/*` 与 C++ 分离，构建部署到 `~/.datahub`（控制面 → `~/.datahub-admin`）

## 快速上手

```bash
# 首次：子模块 workflow
git submodule update --init --recursive && ./ThirdParty/build.sh workflow

# 构建（自动部署前端）
./build.sh --debug DataHub DataHubAdmin

# 运行 —— 须在含 ini 的目录启动（配置按 cwd 读取）
cd DataHub && ../build/debug/datahub          # 数据面 8888
cd DataHubAdmin && ../build/debug/datahub-admin  # 控制面 8899（仅本机）

# 或一键脚本（编译 + WSL 端口转发 + 启动）
cd DataHub && ./run.sh
```

访问：数据面 `http://<服务器IP>:8888/`（选租户/进入后聊天）；控制面 `http://127.0.0.1:8899/`。

## 目录结构

```text
DataHub/
├── main.cpp / run.sh / datahub.ini
├── Application/DataHubApplication.*    # 装配 Tenant/Store/Http 模块
├── Framework/                          # 通用 web:: HTTP 框架（可移植复用）
├── Module/
│   ├── Tenant/                         # ITenantService + CTenantModule（租户/花名册）
│   ├── Storage/                        # IDataStore + DataStoreModule（封装 CFileStore）
│   ├── Http/                           # HttpServerModule + 业务/租户/设备/页面控制器
│   └── Admin/                          # CAdminController（/api/admin/*）
├── Web/                                # tenants.html(.js)/index.html(app.js)/common.js/style.css
└── Linux/Makefile                      # 产物 build/<mode>/datahub + 部署 ~/.datahub

DataHubAdmin/                           # 独立控制面 exe（main.cpp + AdminConsoleModule + Web/）
```

## 文档导航

| 需要… | 看… |
|---|---|
| 项目总览 / 文档导航 | [`docs/datahub/datahub-overview.md`](../docs/datahub/datahub-overview.md) |
| 架构 / 装配 / 请求生命周期 / 扩展 | [`docs/datahub/datahub-architecture.md`](../docs/datahub/datahub-architecture.md) |
| 完整 HTTP API / curl | [`docs/datahub/datahub-http-api.md`](../docs/datahub/datahub-http-api.md) |
| 存储与三套内存模型 / 重启语义 | [`docs/datahub/datahub-storage.md`](../docs/datahub/datahub-storage.md) |
| 前端与对账自愈 | [`docs/datahub/datahub-frontend.md`](../docs/datahub/datahub-frontend.md) |
| 配置键 / 构建 / 排障 | [`docs/datahub/datahub-configuration.md`](../docs/datahub/datahub-configuration.md) |
| 多租户专题（概念→业界→落地→审计） | [`docs/datahub/tenant-*.md`](../docs/datahub/tenant-concepts.md) |

## 说明与边界

- 数据仅存内存，进程退出即清空（如需持久化，给 `IDataStore` 换落盘/DB 实现，见
  `docs/datahub/datahub-storage.md`）。
- 单次上传上限由 `datahub.ini [http] max_body_bytes` 控制（默认 32MB，0 不限）。
- 业务 API 需要设备令牌（先 `POST /api/device/register`）；管理 API 仅本机回环 + 令牌。
- 前端改动无需重编译 C++：重新构建部署或手动拷贝后**重启服务**即可生效。
