# DataHub 租户安全审计（B1–B6）与修复

> 依据 `tenant-concepts.md` / `tenant-industry.md` 中提炼的通用与业界准则，对本项目租户实现
> 做了一次对照审计，发现 **B1–B6** 六项问题。本页记录：发现 → 决策 → 修复落点 → 验证结果。
>
> 应用分组：**DataHub**（数据传输服务）与 **DataHubAdmin**（独立控制面 exe）。

## 1. 审计发现与修复总览

| # | 发现（风险） | 决策 / 修复 | 主要落点 |
|---|---|---|---|
| B1 | **身份可伪造**：`X-Client-Id` 由客户端自报，服务端不校验，任何人都能冒充他人账号读写、删消息 | 引入**每设备令牌（bearer secret）**：设备首次 `POST /api/device/register` 由服务端签发随机令牌；身份相关 API 须同时携带 `X-Client-Id` + `X-Token`，服务端常数时间比对令牌归属 | `CDeviceRegistry.{h,cpp}`、`CDeviceController.{h,cpp}`、`HttpServerModule::OnRequest`、`common.js` |
| B2 | **无"被踢/删除"事件自愈**：租户被删或成员被移除后，客户端缺少可检测端，停在 403/404 | 新增 **`GET /api/tenant/state`** 对账端点（租户不存在也返回 `exists:false`，由 `OnRequest` 放行）；前端每 ~10s 对账：改名→本地刷新，被踢/删除→移出"我的租户"并回落公共 | `TenantsController::HandleState`、`HttpServerModule::OnRequest`、`common.js::reconcileCurrent`、`app.js` |
| B3 | **管理操作无审计**：创建/删除/改名/限流/成员变更不可追溯 | 变更操作统一落 `[Audit] op=… target=… actor=…` 日志：设备侧操作 actor=已认证 clientId；控制面操作 actor=`admin(loopback)` | `TenantsController`（create/join）、`CAdminController`（rename/limits/delete/member） |
| B4 | **业务 API 无显式租户时静默回落 public**：客户端忘记/遗漏 `X-Tenant` 会意外读写公共数据 | 业务数据 `/api/*`（非 `/api/tenant*`、`/api/device*`、`/api/admin*`）**必须显式携带 `X-Tenant`**，缺失回 `400 missing X-Tenant`；公共也须显式传 `public` | `HttpServerModule::OnRequest` |
| B5 | **加入码可枚举、无速率限制**：6 位短码可被盲试枚举私人租户 | join 限流：**≤30 次/分钟/IP**，超出回 `429`；限流与加入结果均审计 | `TenantsController`（匿名 helper `JoinRateLimited`） |
| B6 | **纯内存重启后 404/403 死循环**：服务重启后客户端 localStorage 里的旧租户/旧令牌全部失效 | 两层自愈：① 设备令牌**幂等重注册**（服务端记住则原样返回，否则续发新令牌），`apiFetch` 遇 401 自动重注册重试一次；② `/api/tenant/state` 对账使前端自动清理失效租户并回落公共 | `CDeviceRegistry::Register`、`common.js::ensureDevice/apiFetch/reconcileCurrent`、`app.js` |

## 2. B1 详细：设备身份（从"裸信"到"持物凭据"）

- 原实现把 `X-Client-Id` 当账号且**不做任何校验**；服务端无法区分"本设备"与"冒充者"。
- 修复不引入 WebCrypto/签名，采用**服务端签发、客户端保管的随机令牌**（每设备 32 字符十六进制，128 位熵）：
  1. 首次访问 `POST /api/device/register`（body=`clientId`）→ 服务端 `CDeviceRegistry::Register` 幂等返回令牌；
  2. 之后 `common.js::apiFetch` 自动附加 `X-Client-Id` + `X-Token`；
  3. `OnRequest` 对 `/api/*`（管理、设备注册豁免）先 `CDeviceRegistry::Verify`，不通过回 `401 unauthorized device`，**校验通过后账号才置为已认证 clientId**（彻底去掉 `IP:port` 伪账号回退）。
- 局限（代码注释 + 本文档注明）：令牌是**持物凭据**，在无 TLS 的局域网里若被嗅探即可重放（属 bearer 通病）。本 demo 以内网 + 控制面令牌复用为前提；上公网需换 HTTPS + 短时签名（JWT / HMAC）方案。

## 3. 各请求路径的鉴权总览

| 请求 | 设备令牌 | X-Tenant | 成员闸门 | 说明 |
|---|---|---|---|---|
| `/`、`/chat`、`.css/.js` | — | — | — | 静态页面/资源 |
| `POST /api/device/register` | — | — | — | 注册是第一次握手（幂等） |
| `/api/tenant/*`（create/join/info/members/**state**） | ✅ | 可缺省（state 亦可查"不存在的租户"） | 内部自查 | 租户管理/对账 |
| `/api/text`、`/api/file`、`/api/list`、`/api/members`、`/api/item*` | ✅ | **必须显式** | ✅ | 业务数据（含公共数据） |
| `/api/admin/*` | — | — | — | 仅本机回环 + `X-Admin-Token`（DataHubAdmin 注入） |

## 4. 验证记录

服务端 curl 冒烟（`127.0.0.1:8888`）：

- 无令牌访问业务 API → `401`；带令牌 + `X-Tenant: public` → `200`；
- 带令牌但漏 `X-Tenant` → `400 missing X-Tenant`（B4）；
- 用甲令牌冒充乙 clientId → `401`（B1 防伪造）；
- `/api/tenant/state` 查询不存在的 code → `{"exists":false,...}`（B2）；同名业务请求 → `404`；
- 连续 35 次 join 盲试未知码 → 前 30 次 `404`，随后 `429`（B5）；
- 建租户（owner）→ `state` 显示 `role:"owner"` → 控制面删除 → `state` 变 `exists:false`（B2）；
- 日志出现 `[Audit] op=tenant.create …` 与 `[Audit] op=tenant.delete … actor=admin(loopback)`（B3）。

浏览器端到端（数据面 `:8888` + 控制面 `:8899`）：

- 服务重启后，聊天页原指向旧租户：~3s 内对账检测 `exists:false`，自动移出列表并回落公共页（B6 自愈）；
- 页面创建租户 → 自动注册设备令牌并以 Owner 加入（B1）；进入后发送消息正常落库渲染。

回归：`./build/debug/tests` 81/81 通过。

## 5. 记录在案的取舍

- 令牌表纯内存：重启即清，靠**幂等重注册** + 前端 401 自动续期自愈（B6），不引入持久化密钥库；
- join 限流窗口/阈值（60s / 30 次）为演示级常量，生产应由配置下发并支持分布式计数；
- 成员/角色花名册与 `state` 对账端点同属 `/api/tenant*` 豁免面，仍要求设备令牌，保证调用方身份可溯。
