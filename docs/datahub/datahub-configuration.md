# DataHub 配置 · 构建 · 运行 · 排障

> 面向部署/运维。代码分层与装配见 `datahub-architecture.md`；ini 键都在
> `DataHub/datahub.ini` 与 `DataHubAdmin/datahub-admin.ini` 中有注释。

## 1. 配置加载（重要：按 cwd 读取）

两个服务器都用 ServerCore 配置模块，**从当前工作目录读取 ini**：

```cpp
m_config.LoadFile("datahub.ini");        // CDataHubApplication 构造
m_config.LoadFile("datahub-admin.ini");  // CDataHubAdminApplication 构造
```

> **务必从“含 ini 的目录”启动**：例如 `cd DataHub && ../build/debug/datahub`。若从仓库根目录
> 启动而根目录没有 `datahub.ini`，配置读不到 → 端口/配额/`admin.token` 全回默认值
> （典型症状：`/api/admin/*` 一律 403 `admin forbidden`，因为管理令牌为空）。

### DataHub（数据面）`DataHub/datahub.ini`

| 段.键 | 默认 | 说明 |
|---|---|---|
| `[server] port` | `8888` | 数据面监听端口 |
| `[web] dir` | 空 → `$HOME/.datahub` | 前端静态资源目录（构建自动部署） |
| `[log] level / file` | `info` / `datahub.log` | 日志级别与文件 |
| `[http] max_body_bytes` | `33554432`(32MB) | 单次上传/请求体上限；0=不限 |
| `[admin] token` | 空=不开放 | 管理 API 令牌；须与 DataHubAdmin `[upstream] token` 一致 |
| `[store] max_items` | `0`=不限 | 默认配额：最大条数（每租户） |
| `[store] max_total_mb` | `0`=不限 | 默认配额：内容总容量（MB，每租户） |
| `[store] max_item_mb` | `0`=不限 | 默认配额：单条容量（MB，每租户） |

`[store]` 三项构成新建租户的默认 `CTenantLimits`（配额执行见 `datahub-storage.md` §4）。

### DataHubAdmin（控制面）`DataHubAdmin/datahub-admin.ini`

| 段.键 | 默认 | 说明 |
|---|---|---|
| `[server] port` | `8899` | 控制面监听端口（仅本机回环） |
| `[upstream] base` | `http://127.0.0.1:8888` | 上游数据面基址（代理目标） |
| `[upstream] token` | 空 | 管理令牌，代理时注入 `X-Admin-Token`；须与数据面 `[admin] token` 一致 |
| `[web] dir` | 空 → `$HOME/.datahub-admin` | 管理网页目录 |
| `[log] level / file` | `info` / `datahub-admin.log` | 日志 |

## 2. 构建

### 顶层脚本 `./build.sh`

```bash
./build.sh --debug DataHub              # debug 构建 + 部署前端
./build.sh --release DataHub DataHubAdmin
./build.sh --compiledb DataHub          # 重新生成 compile_commands.json（新增 .cpp 后必刷）
```

- 自动发现含 `Linux/Makefile` 的顶层工程；`Linux/Makefile` 用 `find` 递归收集 `.cpp`
  （排除 `Linux/`），**新增源文件无需改 Makefile**。
- 编译：`-std=c++11 -Wall -Wextra -O0/-O2 -g -pthread`；链接 `libServerCore.a libCommon.a
  libworkflow.a -lcrypto -lpthread`（nossl workflow 内部仍用 OpenSSL crypto 做 base64）。
- 缺库自动递归：`ServerCore/Linux`、`Common/Linux`、`ThirdParty/build.sh workflow`。
- `all` 默认执行 `deploy-web`：把前端拷贝到部署目录。

### 部署目标

| 工程 | 产物 | 前端部署目录 | 拷贝文件 |
|---|---|---|---|
| DataHub | `build/<mode>/datahub` | `$HOME/.datahub` | `index.html style.css app.js common.js tenants.html tenants.js` |
| DataHubAdmin | `build/<mode>/datahub-admin` | `$HOME/.datahub-admin` | `index.html admin.js` |

> `compile_commands.json` 不是普通构建自动生成的（`.gitignore` 忽略）；涉及新增/删除 .cpp 的
> 改动后运行 `./build.sh --compiledb <工程>`，保持 clangd/索引有效（项目纪律）。

## 3. 运行

### 手动（开发）

```bash
cd DataHub && ../build/debug/datahub [port]      # 数据面；默认读 datahub.ini 端口
cd DataHubAdmin && ../build/debug/datahub-admin  # 控制面；默认 8899
```

入口 `main.cpp`：端口可用命令行参数覆盖（`(0,65535]`）；两进程都 `signal(SIGPIPE, SIG_IGN)`。

### 一键 `DataHub/run.sh`

编译(debug/release) + （WSL2 下）自动 `netsh` 端口转发 + 启动 + 自检：

```bash
cd DataHub
./run.sh                  # 编译+外部访问+启动（日志 /tmp/datahub_<端口>.log）
./run.sh --release        # release
./run.sh --no-forward     # 跳过端口转发
./run.sh --stop           # 停止并清理转发
./run.sh -p 9000          # 指定端口
```

> WSL2(NAT) 下局域网设备无法直达 WSL IP，需 Windows 宿主机端口转发——`run.sh` 自动处理。

### 访问

- 设备浏览器：`http://<服务器IP>:8888/`（选择/进入租户后聊天）
- 管理（仅本机）：`http://127.0.0.1:8899/`
- 外部设备直接访问 `8888/api/admin/*` → 403（回环限制）。

## 4. 日志与审计

- 运行日志：ServerCore 日志模块输出（级别见 `[log]`）。开发期常用 `nohup ... > /tmp/dh.log` /
  `/tmp/adm.log` 落盘便于排查。
- **访问日志**：`rid=.. method path tenant=.. -> status ms`（数据面每次请求一行）。
- **审计日志**：变更操作写 `[Audit] op=.. target=.. actor=..`：
  - 设备侧：`tenant.create` / `tenant.join`（含 `tenant.join.ratelimited`），actor=clientId；
  - 控制面：`tenant.delete / rename / limits`、`item.delete`、`member.delete / role`，
    actor=`admin(loopback)`。
  ```bash
  grep '\[Audit\]' /tmp/dh.log
  ```

## 5. 指标（IMetrics，可选）

键：`http.requests`（总量）、`http.<租户码>.requests`（按租户请求量）、`http.members`
(gauge：当前租户在线数)。未装配指标模块不影响服务（可选依赖）。

## 6. 回归测试

```bash
./build.sh --debug Common DataHub DataHubAdmin   # 数据面/控制面（Tests 走 Common/ServerCore 等）
./build/debug/tests                              # 期望 81/81
```

## 7. 常见排障

| 症状 | 原因 | 处理 |
|---|---|---|
| `/api/admin/*` 一律 403 `admin forbidden`（即使本机+带令牌） | 从无 ini 的目录启动 → `admin.token` 为空 | `cd DataHub` 再启动（§1） |
| 页面 503 `前端页面未加载` | `~/.datahub`（或自定义 web.dir）里文件缺失 | 重新 `./build.sh --debug DataHub` 部署 |
| 业务请求 401 | 设备令牌失效（服务重启） | 前端自动续期；手动 curl 先 `POST /api/device/register` |
| 业务请求 400 `missing X-Tenant` | 忘了显式 `X-Tenant`（B4） | 显式传 `X-Tenant: public` 或目标租户码 |
| 聊天 404 `tenant not found` 循环 | 本地缓存了已删租户（重启后最常见） | 前端 `/api/tenant/state` 对账自动回落（B6） |
| join 429 | 短时间盲试过多（B5：≤30/分/IP） | 等窗口；确认分享码 |
| `端口可能被占用` | 端口已被占 | `run.sh --stop` / 换 `-p` |
| 大文件下载失败/截断 | 单次超大响应在部分网络被截断 | 前端已按 64KB Range 分段；检查客户端是否走分段拉取 |
| 改完前端不生效 | 资源启动时一次性读入内存 | 重新部署 + 重启服务（§3） |

## 8. 相关文档

- 构建系统全局约定：仓库根 `.github/skills/build-system/`、`README.md`
- 依赖库（workflow/Common）：`.github/skills/dependency-management/`
- 端点与配额语义：`datahub-http-api.md`、`datahub-storage.md`
