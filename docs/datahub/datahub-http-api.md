# DataHub HTTP API 参考

> 面向调用方/集成方。基址：数据面 `http://<服务器IP>:8888`（默认）；管理域仅经控制面
> `http://127.0.0.1:8899` 访问（代理）。鉴权语义（B1/B4 等）见 `tenant-audit.md`，
> 请求如何被路由/裁决见 `datahub-architecture.md` §3。

## 1. 请求头与鉴权域

| Header | 用途 | 谁带 |
|---|---|---|
| `X-Tenant` | 目标租户码（如 `public`）。**业务数据 API 必带**，缺失回 400；`/api/tenant*` 可缺省 | 设备 |
| `X-Client-Id` | 设备/账号标识（浏览器持久化 UUID） | 设备 |
| `X-Token` | 设备令牌（`/api/device/register` 签发）。`/api/*`（管理、register 除外）**必带** | 设备 |
| `X-Admin-Token` | 管理令牌。**只在控制面配置里**，由 DataHubAdmin 代理注入，浏览器永不携带 | 控制面 |

两个鉴权域，互不重叠：

- **设备域**（数据面对外）：`X-Client-Id` + `X-Token` 校验身份；再按 `X-Tenant` 判成员。
- **管理域**（仅本机回环 + `X-Admin-Token`）：`/api/admin/*` 只经 DataHubAdmin(8899) 代理到达
  数据面(8888) 的回环接口；外部设备访问 `8888/api/admin/*` 一律 403。

### 常见错误 JSON 与状态码

| 状态码 | 典型 `{"error": ...}` | 触发 |
|---|---|---|
| 400 | `missing X-Tenant: choose a tenant` / `empty body` / `missing code` | 缺显式租户、空 body、缺参数 |
| 401 | `unauthorized device` | 设备令牌缺失/不匹配（B1） |
| 403 | `admin forbidden` / `not a tenant member` / `forbidden` | 管理域回环/令牌失败、非成员、越权删数据 |
| 404 | `tenant not found` / `not found` / 前端缺失 | 未知租户、id/资源不存在 |
| 413 | `body too large` | 超过 `[http] max_body_bytes`（默认 32MB） |
| 429 | `too many join attempts` | join 限流（B5：≤30 次/分/IP） |
| 500 | `... unavailable` / `save failed` | 服务未就绪/内部失败 |
| 502 | `upstream unreachable` | 控制面代理到数据面失败 |
| 503 | `前端页面未加载` 等 | 前端资源未部署、上游未配置 |

> 空 `body` 的文本 POST 会被 workflow 拒绝而回 400；上传/请求体**超限**回 413。

## 2. 页面路由（GET，无需令牌/租户）

| 路径 | 文件 | 说明 |
|---|---|---|
| `/` | `tenants.html` | 根页：租户选择/创建/加入/进入 |
| `/chat` | `index.html` | 聊天室（当前租户） |
| `/style.css` `/app.js` `/common.js` `/tenants.js` | 对应文件 | 静态资源 |

前端资源在服务启动时一次性读入内存，带 `ETag`（FNV-1a），命中 `If-None-Match` 回 304。

## 3. 设备凭据（免租户；仅 `POST`，正文=clientId）

**`POST /api/device/register`**（body=`<clientId>`，幂等）

```text
200  {"token":"<32位随机令牌>"}          # 已注册则原样返回既有令牌
400  {"error":"invalid client id"}       # 长度 <8 或 >64
```

- 这是第一次握手的唯一免令牌端点；此后业务请求带 `X-Token`。
- 服务重启令牌表清空 → 客户端**幂等重注册**续发新令牌（前端 `apiFetch` 遇 401 自动续期）。

## 4. 租户管理（`/api/tenant*`，需设备令牌；可缺省 X-Tenant）

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/tenant` | 创建租户（body=名称）；创建者为 Owner |
| GET | `/api/tenant/info?code=` | 凭码加入前校验存在性 → `{code,name,created}` |
| POST | `/api/tenant/join` | 凭码加入（body=码；缺省取 X-Tenant→public） |
| GET | `/api/tenant/members` | 当前（X-Tenant）花名册 |
| GET | `/api/tenant/state` | 当前（X-Tenant）状态对账（**租户不存在也 200**） |

```text
POST /api/tenant           200 {"code":"Ab3xYz","name":"...","role":"owner"}
GET  /api/tenant/info?code=Ab3xYz   200 {"code":"Ab3xYz","name":"...","created":<ms>}
POST /api/tenant/join      200 {"role":"owner"|"member"}        # 已加入返回原角色
                           404 {"error":"tenant not found"}      # 码不存在
                           429 {"error":"too many join attempts"}# 限流(B5)
GET  /api/tenant/members   200 {"members":[{"account":"<clientId>","role":"owner|member","joined":<ms>}]}
                            # 公共租户花名册为空；非成员查询私有租户花名册 → 403
GET  /api/tenant/state     200 {"code":"...","name":"...","exists":bool,
                                "isDefault":bool,"role":"owner|member|"}
```

**`state` 语义（B2/B6 对账）**：`exists:false` = 租户已被删；`role` 空且非公共 = 已被移除成员。
前端据此**自动移出“我的租户”并回落公共**，避免 404/403 死循环；改名则用 `name` 刷新本地缓存。
创建/加入成功与 join 限流都写 `[Audit]` 日志。

## 5. 业务数据（`/api/text /api/file /api/list /api/members /api/item*`）

> 需设备令牌 + **显式 `X-Tenant`**（B4）。所有条目按租户隔离（共享表 + 行级 tenant）。

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/text` | 发文本：body=内容 → `{"id":...}` |
| GET | `/api/text/{id}` | 取文本（`text/plain`） |
| POST | `/api/file` | 上传文件：header `X-File-Name`(URL 编码) + body → `{"id":...}` |
| GET | `/api/file/{id}` | 下载/预览文件（图片内联；附件带 Content-Disposition；支持 Range） |
| GET | `/api/list?since=<seq>` | 消息列表（游标增量；缺省全量） |
| GET | `/api/members` | 在线成员（30s TTL，按最后活跃倒序） |
| DELETE | `/api/item/{id}` | 删除条目（见权限） |

```text
POST /api/text      200 {"id":"<短码>"}   400 empty body / 413 too large
GET  /api/text/{id} 200 <纯文本>          404 {"error":"not found"}
POST /api/file      200 {"id":"<短码>"}   header X-File-Name（文件名，URL 编码后传）
GET  /api/list      200 {"items":[{"id","type":"text|file","name","from","size","seq","time"}]}
                        # since=<seq>：只返回 seq>since 的新增项（升序 旧→新）
GET  /api/members   200 {"members":[{"id","ip","first","last"}]}
DELETE /api/item/{id} 200 {"ok":true}    403 forbidden / 404 not found
```

**`/api/list` 与 seq**：数据项带全局单调递增 `seq`；客户端以 `lastSeq` 为游标
`GET /api/list?since=lastSeq` 增量拉新，历史增长不放大轮询负载（缺省/`since=0` 全量）。

**`GET /api/file/{id}` 下载语义**（逻辑在 `CHttpResponse::WriteFile`）：

- 图片（后缀命中 image/*，如 png/jpg/gif/webp/bmp/svg/ico）→ `Content-Disposition: inline`，
  浏览器内联显示；
- 其它 → 附件下载；非 ASCII 文件名用 `filename*=UTF-8''...`（RFC 5987），否则普通 `filename=`;
- 带 `Range: bytes=S-E`（或 `bytes=S-`）且 `start<total` → 回 **206** + `Content-Range`；
  越界/解析失败/无 Range → 200 全量（不返回 416）。大文件客户端按段拉取再拼接，规避
  workflow 对单次超大响应体在部分网络环境的截断（前端按 64KB/段）。

**`DELETE /api/item/{id}` 权限**（判定方 = 请求上下文已认证账号 vs 条目 `strFrom`）：

- Owner：可删该租户任意数据；
- Member / 非 Owner：仅可删 `strFrom == 自己` 的条目，否则 **403**；
- 条目 `strFrom` 为空时任何人可删（兼容旧数据）。

## 6. 管理 API（`/api/admin/*`，经 DataHubAdmin 代理，本机 + 令牌）

> 直接 curl 数据面 `8888/api/admin/*` 需在**本机回环**且带 `X-Admin-Token`；正常使用路径是
> 浏览器打开 `http://127.0.0.1:8899/`，其 `admin.js` 调用的就是下表路径（代理自动注入令牌）。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/admin/overview` | 租户一览 + 汇总（count/totalItems/totalBytes/tenants[]） |
| GET | `/api/admin/tenant?code=` | 单租户详情：信息 + 成员 + 数据条目 |
| DELETE | `/api/admin/tenant?code=` | 删除租户（公共不可删；连带清数据） |
| POST | `/api/admin/tenant/rename` | 改名（表单 code/name） |
| POST | `/api/admin/tenant/limits` | 配额（表单 code/maxItems/maxTotalBytes/maxItemBytes） |
| GET | `/api/admin/item?code=&id=` | 条目元信息；文本附内容预览（截断 2000 字符） |
| DELETE | `/api/admin/item?code=&id=` | 删除条目 |
| DELETE | `/api/admin/member?code=&account=` | 移除成员（须保留 ≥1 Owner） |
| POST | `/api/admin/member/role` | 设角色 owner/member（表单 code/account/role） |

管理域成功回 `{"ok":true}`（GET 详情/概览回数据 JSON）；失败回错误 JSON，语义同 §1 表。
每次**变更操作落 `[Audit]`**（actor=`admin(loopback)`）。公共租户不可删、不可做成员管理；
公共租户改名允许。配额字段 0 = 不限制（详情接口输出与 `[store]` 默认一致）。

## 7. 端到端 curl 示例（数据面）

```bash
B=http://127.0.0.1:8888
CID=dev-$(cat /proc/sys/kernel/random/uuid)

# 1) 设备注册（幂等，拿令牌）
TOK=$(curl -s -X POST $B/api/device/register -d "$CID" | sed -E 's/.*"token":"([^"]+)".*/\1/')
# 2) 公共租户读写（业务 API：必须显式 X-Tenant）
curl -s -X POST -H "X-Client-Id: $CID" -H "X-Token: $TOK" -H "X-Tenant: public" \
     -d '你好' $B/api/text
curl -s -H "X-Client-Id: $CID" -H "X-Token: $TOK" -H "X-Tenant: public" \
     "$B/api/list?since=0"
# 3) 建租户 → 查 state → 进入
CR=$(curl -s -X POST -H "X-Client-Id: $CID" -H "X-Token: $TOK" $B/api/tenant -d '产线组')
CODE=$(echo "$CR" | sed -E 's/.*"code":"([^"]+)".*/\1/')
curl -s -H "X-Client-Id: $CID" -H "X-Token: $TOK" -H "X-Tenant: $CODE" $B/api/tenant/state
# 4) 在私有租户发文件（显式 X-Tenant）
curl -s -X POST -H "X-Client-Id: $CID" -H "X-Token: $TOK" -H "X-Tenant: $CODE" \
     -H 'X-File-Name: photo.jpg' --data-binary @photo.jpg $B/api/file
```

管理面 curl（需在数据面所在主机本机执行）：

```bash
curl -s -H 'X-Admin-Token: <与 datahub-admin.ini 一致>' \
     'http://127.0.0.1:8888/api/admin/overview'
```

## 8. 相关文档

- 鉴权动机与 B1–B6：`tenant-audit.md`
- 租户实体/角色/花名册落地：`tenant-project.md`、`datahub-storage.md`
- 前端如何调用这些端点：`datahub-frontend.md`
- 配置键（max_body_bytes、admin.token 等）：`datahub-configuration.md`
