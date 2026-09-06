# DataHub 架构（分层 · 模块装配 · 请求生命周期 · 线程 · 扩展）

> 面向想读懂/修改 DataHub 代码的开发者。HTTP 端点与语义见 `datahub-http-api.md`，
> 三套内存模型见 `datahub-storage.md`。控制面 DataHubAdmin 的代理架构单独成节。

## 1. 分层总览

DataHub 代码自上而下依赖四层，各层职责清晰、可复用边界明确：

```text
┌─────────────────────────────────────────────────────────────────┐
│ ① 应用层   DataHub/main.cpp → Application/DataHubApplication      │
│           装配 ServerCore 模块；读 datahub.ini                     │
├─────────────────────────────────────────────────────────────────┤
│ ② 业务层   Module/{Tenant, Storage, Http, Admin}                 │
│           接口(ITenantService/IDataStore) + ServerCore 模块实现，   │
│           控制器(无状态) + 普通服务(CMemberService/CDeviceRegistry) │
├─────────────────────────────────────────────────────────────────┤
│ ③ 框架层   Framework/  (web::)                                    │
│           CHttpMessage/CHttpRouter/CHttpText —— 只依赖 Workflow    │
├─────────────────────────────────────────────────────────────────┤
│ ④ 基础库   ServerCore(骨架/DI/日志/指标/配置) · Common(CFileStore)  │
│           ThirdParty/Workflow(nossl) —— 被 ③ 直接依赖               │
└─────────────────────────────────────────────────────────────────┘
```

要点：

- **业务不接触 workflow**：`WFHttpTask` 被 `Framework/HttpMessage` 收口，控制器只见
  `web::CHttpRequest / CHttpResponse`，可在其它 Workflow 项目间整体复用（见
  `Framework/README.md` 移植步骤）。
- **接口+模块 = ServerCore 玩法**：可跨模块复用的能力（租户、存储）都声明为 sc 命名空间
  接口，由 ServerCore `CModule` 子类按接口注册；HTTP 模块在初始化时经依赖注入 `Resolve`。
- **控制器（Controller）不是模块**：`CHttpHandlers / CTenantsController / CAdminController /
  CDeviceController / CWebPageController` 都是普通类，由 `CHttpServerModule` 持有并注入依赖，
  生命周期归装配层（模块），避免把每个路由组都做成模块。

## 2. 模块装配（DataHubApplication::RegisterModules）

顺序即初始化/启动顺序（`DataHub/Application/DataHubApplication.cpp`）：

```text
CMyApplication::RegisterModules()   # ① 基类默认：IConfig / ILogger / IMetrics
  ↓
CTenantModule(defaultLimits)        # ② 按 IID_ITenantService 注册（含内置 public 租户）
  ↓                                  默认配额来自 datahub.ini [store]（0=不限）
CDataStoreModule()                  # ③ 按 IID_IDataStore 注册（内部封装 Common CFileStore）
  ↓
CHttpServerModule(port, webDir,     # ④ HTTP 模块（声明依赖 IDataStore / ITenantService，
    maxBodyBytes, adminToken)         由 CModuleManager 拓扑排序保证先就绪）
```

`CHttpServerModule::Initialize(ctx)` 的装配（`DataHub/Module/Http/HttpServerModule.cpp`）：

| 成员 | 类型 | 注入/职责 |
|---|---|---|
| `m_pStore` | `ScopedInterfacePtr<IDataStore>` | `ctx.Resolve`，缺失则初始化失败 |
| `m_pTenants` | `ScopedInterfacePtr<ITenantService>` | `ctx.Resolve`，缺失则初始化失败 |
| `m_pMetrics` | `ScopedInterfacePtr<IMetrics>` | 可选依赖，未装配不影响启动 |
| `m_pMembers` | `unique_ptr<CMemberService>` | 在线活跃（30s TTL） |
| `m_pHandlers` | `unique_ptr<CHttpHandlers>` | 业务端点（list/text/file/members/item） |
| `m_pTenantCtl` | `unique_ptr<CTenantsController>` | 租户管理端点（/api/tenant/*） |
| `m_pPages` | `unique_ptr<CWebPageController>` | 前端页面/静态资源（启动一次性读入内存） |
| `m_pDevices` | `unique_ptr<CDeviceRegistry>` | 设备令牌表（clientId → token） |
| `m_pDeviceCtl` | `unique_ptr<CDeviceController>` | `/api/device/register` |
| `m_pAdminCtl` | `unique_ptr<CAdminController>` | 管理端点（/api/admin/*）→ 挂到 `m_routerAdmin` |

路由注册：业务/租户/设备/页面控制器都注册进**同一个 `m_router`**（各自 `RegisterRoutes`），
管理控制器注册进**独立的 `m_routerAdmin`**——两套路由在 `OnRequest` 里被不同闸门分流。

### 控制面装配（DataHubAdmin）

`CDataHubAdminApplication::RegisterModules` 只装配基类默认模块 + `CAdminConsoleModule`：
监听回环 `8899`，读 `upstream.base/token` 与 `web.dir`。模块内建 `WFHttpServer`，
`OnRequest` 流程：**回环校验 → /api/admin* 代理上游 → 其它 ServePage**。详见 §6。

## 3. 数据面请求生命周期（OnRequest 流水线）

`CHttpServerModule::OnRequest` 是数据面所有 HTTP 请求的统一入口（Workflow 线程池回调），
按路径分类逐级裁决：

```text
进入 OnRequest
 ├─ 分类：/api/admin* | /api(非 register) | 其它(页面/register)
 │
 ├─(A) 管理 /api/admin*   → 回环? + X-Admin-Token 匹配?
 │     否则 403 admin forbidden；通过 → m_routerAdmin.Dispatch
 │
 ├─(B) 业务 / 设备 API（/api 且非 register）
 │     ① 设备鉴权：X-Client-Id + X-Token ∈ CDeviceRegistry?  否则 401
 │       通过 → ctx.strAccountId = 该 clientId（去掉 IP:port 伪账号回退）
 │     ② B4 显式租户：非 /api/tenant* 必须带 X-Tenant          否则 400
 │     ③ 解析：FindTenant(strCode) → ctx.tenant
 │        ├─ 未知租户 && 恰为 /api/tenant/state → 放行(返回 exists:false)
 │        ├─ 未知租户（其它）        → 404 tenant not found
 │        └─ 已知：public 或 /api/tenant* 或 花名册角色命中 → 通过
 │           否则 403 not a tenant member
 │     ④ 通过：Touch 在线成员 → 上报指标 → m_router.Dispatch
 │
 └─(C) 其它（页面/静态 /api/device/register）
        默认标记 public 上下文 → m_router.Dispatch
 统一收尾：按状态码分类 + 写访问日志（rid / method / path / tenant / status / ms）
```

要点：

- **请求级上下文** `CRequestContext{tenant, strAccountId, strRequestId, bResolved}` 是
  `OnRequest` 栈上局部对象，经 `req.SetUserData(&ctx)` 挂到请求；控制器用
  `RequestContextOf(req)` 读取（为空返回静态空对象兜底）。生命周期＝当前请求栈。
- **身份/账号/成员分层**：`CDeviceRegistry` 证明“设备是谁”（B1），`CRequestContext` 携带
  已认证账号，`CTenantModule::TenantRoleOf` 决定“能不能进这个租户/删这条数据”。
- **B4 显式租户**：业务数据 API 若漏带 `X-Tenant` 直接 400，杜绝“静默落到 public”。
- 安全审计（B1–B6）的完整动机与修复见 `tenant-audit.md`。

## 4. 线程模型

| 对象 | 线程安全实现 |
|---|---|
| `CTenantModule`（租户/花名册） | 单把 `std::mutex m_mutex` 串行化全部公开方法 |
| `DataStoreModule` → `CFileStore` | 存储内部自锁，线程安全；控制器不额外加锁 |
| `CMemberService`（在线活跃） | 单把 `mutex` 串行化 |
| `CDeviceRegistry`（设备令牌） | 单把 `mutex`；`Verify` 常数时间比较防时序侧信道 |
| `web::CHttpRouter` | 注册与分发可跨线程 |
| 指标 `IMetrics` | 服务器自带原子/线程安全上报 |

所有请求在 **Workflow 线程池**中并发执行；由于存储层都自锁，控制器代码无需再考虑锁。
没有独立业务线程：数据面没有后台定时器（在线成员 TTL 在 `GET /api/members` 里被动 `Prune`）。

## 5. 指标与日志

- 指标键（`IMetrics`）：`http.requests`（总请求）、`http.<租户码>.requests`（按租户）、
  `http.members`（gauge，当前租户在线数）。
- 访问日志：`rid=.. method path tenant=.. -> status ms`；每次请求一个请求 id。
- 审计日志：`[Audit] op=.. target=.. actor=..`（`datahub-configuration.md` §日志）。
- 日志/指标分别由 ServerCore 的 `CLoggerModule / CMetricsModule` 提供，属基类默认装配。

## 6. 控制面代理架构（DataHubAdmin）

控制面与数据面分离的核心约束与实现：

- **只本机可达**：`CAdminConsoleModule::OnRequest` 先 `IsLoopbackPeer`（仅 `127.0.0.1`/`::1`），
  否则 403。
- **页面与 API 分流**：`/api/admin*` → `ProxyToUpstream`；其余 → `ServePage`
  （`/` 或 `/index.html` → HTML，`/admin.js` → JS；缺文件 503，其它 404）。
- **代理规则**（`ProxyToUpstream`）：
  - 上游 URL = `upstream.base + uri`（**原样带 query**），`create_http_task` + 保留原方法；
  - 复制请求头：遍历跳过 `Host/Content-Length/Transfer-Encoding/Connection/Accept-Encoding`；
  - **注入 `X-Admin-Token`**（令牌只在控制面配置里，浏览器永不接触）；
  - 复制请求体（POST 表单等）；
  - `series_of(serverTask)->push_back(clientTask)`：保证上游完成后才回包；
  - `OnUpstreamReply`：上游成功 → `*serverResp = std::move(*clientResp)` 整包搬回；
    失败 → `502 {"error":"upstream unreachable"}`。
- 因此浏览器只会“看到”控制面 `8899`，数据面的 `/api/admin/*` 只认**回环来源**——即使控制面
  被远程访问，回环限制也会拦下它。

## 7. 扩展点（怎么加东西）

| 想做什么 | 做法 | 位置 |
|---|---|---|
| 新增前端页面/静态资源 | 在 `CWebPageController` 页表加一行（route→file→mime→缺失行为） | `Http/WebPageController.cpp` |
| 新增 HTTP 端点 | 给对应控制器加 handler 并在其 `RegisterRoutes` 注册；若属新 API 域，另起控制器由 `HttpServerModule::Initialize` 装配 | `Http/`、`Admin/` |
| 新增业务能力（可被多项目复用） | 定义 sc 接口（`SC_INTERFACE`）+ 写 ServerCore 模块实现，`RegisterModule` 装配，HTTP 侧 `ctx.Resolve` | `Module/`、`Application/` |
| 扩展存储（落盘/换后端） | 实现另一份 `IDataStore`（数据面替换 `DataStoreModule`）；文件格式/共享表在 Common `CFileStore` | `Storage/`、`Common/Storage` |
| 新增独立服务器（如新控制面） | 仿 DataHubAdmin：`main.cpp` + `CMyApplication` 子类 + 一个 ServerCore 模块 | 新建顶层工程目录 + `Linux/Makefile` |
| 改权限/闸门 | 集中在 `OnRequest`（进租户）与 `HttpHandlers::HandleDelete`（删数据）；角色判定在 `CTenantModule` | `Http/HttpServerModule.cpp` |

> 新增 `.cpp` 会被 `Linux/Makefile` 的 `find` 自动收集（无需改 Makefile）；但
> `compile_commands.json` 不会自动更新——改动后运行 `./build.sh --compiledb <工程>`。
