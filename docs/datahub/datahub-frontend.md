# DataHub 前端

> DataHub 前端是与 C++ 分离的**静态资源**（构建时部署到 `~/.datahub`，启动一次性读入内存，
> 带 ETag/304）。数据面有两页：根页 `/`（租户选择/管理）与 `/chat`（聊天室）；控制面
> DataHubAdmin 另有独立管理网页。本文覆盖三者的文件组织、共享状态、鉴权与对账逻辑。

## 1. 文件与页面映射

| 页面 | URL | 文件 | 职责 |
|---|---|---|---|
| 租户选择/管理 | `/` | `tenants.html` + `tenants.js` | 创建/凭码加入/我的租户/进入/移出 |
| 聊天室 | `/chat` | `index.html` + `app.js` | 当前租户内收发文本/文件、在线成员 |
| 公共 | — | `common.js` | 两页共享：CLIENT_ID、租户状态、设备令牌、apiFetch |
| 样式 | — | `style.css` | 两页共用 |
| 管理控制面 | `127.0.0.1:8899/` | DataHubAdmin `Web/index.html` + `admin.js` | 租户/成员/数据管理（全经代理） |

页面与静态资源的路由注册在 `CWebPageController` 页表（`Module/Http/WebPageController.cpp`）：
新增页面 = 表里加一行。构建部署目标：DataHub → `$HOME/.datahub`，DataHubAdmin →
`$HOME/.datahub-admin`。

## 2. common.js：共享状态与账号

`common.js` 定义全局 `window.DH`，两页共用同一账号与“我的租户/当前租户”本地状态
（localStorage 持久化，跨页面一致）：

| 项 | localStorage | 说明 |
|---|---|---|
| `CLIENT_ID` | `datahub_client_id` | 浏览器持久化 UUID（X-Client-Id） |
| `DH.tenants` | `datahub_tenants` | “我的租户”列表 `[{code,name}]` |
| `DH.currentCode/currentName` | `datahub_current_tenant` | 当前聊天租户（缺省 `public`） |
| `DH.token` | `datahub_device_token` | 设备令牌（X-Token，B1） |

关键行为：

- **设备令牌注册 `DH.ensureDevice()`**：无令牌（或 `force`）时 `POST /api/device/register`
  (body=`CLIENT_ID`) 拿令牌存 localStorage；服务端记住则幂等原样返回 → 天然支持“重启续期”。
- **`DH.apiFetch(url, options)` 返回 Promise**：自动附加 `X-Client-Id`；`X-Tenant` 缺省填
  `currentCode`（调用方可用 headers 覆写，如按某租户查角色）；先 `ensureDevice()` 再带
  `X-Token`；收到 **401 自动重注册重试一次**（B6 防 stale 死循环）。
- **租户状态对账 `DH.reconcileCurrent()`**：对当前私有租户 `GET /api/tenant/state`——
  - `exists:false`（被删）或 `role` 空（被踢）→ `removeTenant` 移出列表并回落 `public`，
    返回 `{changed:true}`；
  - 名变更 → `addTenant` 刷新缓存名，返回 `{changed:false, renamed:true, name}`。

## 3. 根页（tenants.html / tenants.js）：租户选择与加入

四个区块：当前聊天租户提示 → **创建租户**（名称 maxlength=48，创建即 Owner，可凭码邀请）→
**凭码加入**（6 位码 maxlength=6，加入即成员）→ **我的租户**（“进入”开始聊天；“移出”离开）。

- 创建：`POST /api/tenant`（body=名称）→ 成功 `DH.addTenant(code,name)` 并置为当前；
- 凭码加入：先 `GET /api/tenant/info?code=` 校验存在，再 `POST /api/tenant/join`（body=码），
  成功后加入“我的租户”；
- 角色标签：列表每项按租户拉 `GET /api/tenant/members`（覆写 `X-Tenant`）得到当前账号角色
  显示 `Owner / 成员 / 开放`（公共租户为“开放”）；
- 移出：`removeTenant`（本地列表，不回写服务端——服务端移除成员是 Owner/控制面行为）。
- 页面顶部“去聊天室”直接链到 `/chat`；进入某租户 `DH.setCurrent(code)` 后跳到 `/chat`。

## 4. 聊天页（index.html / app.js）：当前租户内的实时互传

- **租户栏固定**：页面顶部显示“🏢 租户 + 当前租户名”，旁有“管理 / 切换 ▸”链回 `/`。
  聊天室**不在页内切租户**（租户 = 有边界组织，进别的租户须回根页选），语义见
  `tenant-project.md`。
- **幂等加入**：`initTenantBar()` 在非公共租户时先 `POST /api/tenant/join`（幂等，确保刷新/直链
  进入时仍是成员）并显示当前名。
- **轮询同步（游标增量）**：每 `POLL_MS=2000ms` 一次 `GET /api/list?since=lastSeq` 只拉新；
  消息到达更新游标并按需 `GET /api/text/{id}` 取正文渲染。文本/图片 `renderMsg` 本地即时
  上屏（乐观渲染，自己消息带“删除”）。
- **在线成员**：`GET /api/members` 后台每 2s 更新计数（“N 人在线”）；点“查看成员”展开
  `IP:port` + 最后活跃列表。
- **发送**：文本 `POST /api/text`；文件 `POST /api/file`（`X-File-Name` URL 编码 + body 二进制）。
- **图片/文件**：图片内联预览（点击放大 `#overlay`），其余文件卡片可下载；因为 workflow 在
  部分网络环境会截断单次超大响应体，**大文件按 64KB 段 Range 拉取再拼接**
  （`fetchFile`：先 `Range: bytes=0-0` 探 `Content-Range` 总长，再逐段 `bytes=S-E`），
  每段小于截断阈值，保证可靠下载。
- **删除**：自己消息调 `DELETE /api/item/{id}`；成功后移除 DOM 节点。
- **对账自愈（B2/B6）**：进页 3s 后首查、此后每 10s 调 `DH.reconcileCurrent()`；检测到被踢/
  被删 → toast 提示并跳回 `/`（此时本地已清理）；检测到改名 → 原位刷新租户栏标题。

## 5. 控制面管理网页（DataHubAdmin/Web）

`index.html`（内联 CSS）+ `admin.js`（IIFE），全部请求**打给同源 `/api/admin/*`**，
由控制面 `CAdminConsoleModule` 代理到数据面并注入 `X-Admin-Token`（浏览器永远看不到令牌）。

- 顶栏：标题 + 刷新按钮；overview 卡片（租户数 / 数据条目 / 占用字节）。
- 「全部租户」列表：每行 name / code / 成员数 / 条目数 / bytes / 配额 / 公共徽标 + “详情”。
- 「租户详情」`showTenant(code)` → `GET /api/admin/tenant?code=`：
  - **成员**：account、role 徽标、加入时间；Owner 之外可“移除”；角色下拉可升/降级
    （`POST /api/admin/member/role`，Owner 变更须保留 ≥1 Owner）；
  - **数据条目**：kind（文本💬/文件📎）、name、id、from、size/time；文本条目“预览”
    （`GET /api/admin/item?code&id`，正文截断 2000 字符），全部可“删除”；
  - **租户操作**：公共租户仅改名；非公共还有“配额表单”（maxItems / 总 MB / 单条 MB）与
    “删除该租户（含数据）”（带 confirm，走 `DELETE /api/admin/tenant?code`）。
- 暴露全局 `window.__*` 供内联 onclick 使用；`loadOverview()` 启动即执行。

## 6. 前端→API 对照速查

| 前端动作 | HTTP 调用 |
|---|---|
| 设备注册/续期 | `POST /api/device/register` |
| 创建租户 / 凭码校验 / 加入 | `POST /api/tenant`、`GET /api/tenant/info?code=`、`POST /api/tenant/join` |
| 拉角色标签 | `GET /api/tenant/members`（覆写 X-Tenant） |
| 对账（改名/被踢/被删） | `GET /api/tenant/state` |
| 收/发消息 | `GET /api/list?since=`、`POST /api/text`、`GET /api/text/{id}` |
| 文件上传/下载 | `POST /api/file`、`GET /api/file/{id}`（Range 分段） |
| 在线成员 | `GET /api/members` |
| 删自己消息 | `DELETE /api/item/{id}` |
| 管理面（控制面网页） | `/api/admin/overview | tenant | rename | limits | item | member…` |

## 7. 开发与调试

- 改 HTML/JS 后**无需重编译 C++**：重新 `./build.sh --debug DataHub DataHubAdmin`（会部署到
  `~/.datahub`、`~/.datahub-admin`）或手动拷文件后**重启服务**（启动时一次性读入内存）。
- 浏览器强缓存已用 ETag/`If-None-Match` 304 协调；开发时可临时禁用缓存或改 ETag 内容。
- 前端无构建链（原生 JS + fetch），无包管理器依赖。
