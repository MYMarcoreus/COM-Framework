# DataHub —— 设备间 HTTP 数据传输服务

基于 **ServerCore 骨架 + Sogou Workflow** 的精简 HTTP 数据传输服务。
手机、电脑等设备接入同一网络后，通过浏览器即可互传文本与文件。

## 特性

- **ServerCore 骨架**：复用 `CMyApplication` 生命周期 + 模块模型（`CModuleManager`）+ 配置/日志/指标
- **Workflow HTTP**：`WFHttpServer` 高性能异步服务，封装为 ServerCore 模块（`CHttpServerModule`）
- **纯内存存储**：数据项存内存，提取码为 6 位随机码（去掉易混淆字符）
- **独立前端资源**：`Web/index.html` 与 C++ 代码分离，运行时从磁盘加载，便于独立维护与部署
- **精简 API**：上传 / 下载 / 列表 / 删除

## 架构

```text
main.cpp
  ↓
CDataHubApplication (ServerCore CMyApplication)
  ├── CConfigModule / CLoggerModule / CMetricsModule   # 基类默认装配
  ├── CDataStoreModule        # IDataStore：内存存储（文本 / 文件）
  └── CHttpServerModule       # IHttpService：封装 Workflow WFHttpServer
```

- `CDataStoreModule` 按接口注册（`IID_IDataStore`），供 HTTP 模块按接口解析（依赖注入）。
- `CHttpServerModule` 声明依赖 `IDataStore`，由 `CModuleManager` 拓扑排序保证先就绪。
- HTTP 回调为 Workflow 线程池执行，通过接口访问数据存储（存储内部加锁，线程安全）。

## 一键脚本（推荐）

```bash
cd DataHub
./run.sh                # 编译(debug) + 配置外部访问 + 启动
./run.sh --release      # 编译(release) + 配置外部访问 + 启动
./run.sh --no-forward   # 跳过端口转发（纯本机运行）
./run.sh --stop         # 停止服务器并清理端口转发
./run.sh -p 9000        # 指定端口（默认从 datahub.ini 读取）
```

脚本会自动完成：
1. **编译**：调用根目录 `./build.sh` 构建 DataHub，并部署前端页面到 `~/.datahub/`
2. **配置外部访问**：检测到 WSL2 环境时，自动调用 Windows `netsh` 添加端口转发（Windows 端口 → WSL IP 端口），并放行防火墙
3. **启动**：后台启动 datahub 并输出访问地址（本机 + 局域网，供手机访问）
4. **自检**：确认进程存活，日志写入 `/tmp/datahub_<端口>.log`

> WSL2 环境（NAT 模式）下，手机等局域网设备无法直接访问 WSL 内部 IP，需经 Windows 宿主机端口转发——`run.sh` 自动处理，无需手动执行 netsh 命令。

## 构建

```bash
# 1. 初始化子模块并编译 workflow 静态库（首次）
git submodule update --init --recursive
./ThirdParty/build.sh workflow

# 2. 构建（构建时自动把前端页面部署到用户目录 ~/.datahub/index.html）
./build.sh DataHub            # debug（默认）
./build.sh --release DataHub  # release
```

## 运行（手动）

```bash
# 从任意目录运行均可（前端页面从用户目录 ~/.datahub/index.html 读取）
./build/debug/datahub

# 或指定端口
./build/debug/datahub 9000
```

启动后，同网络内任何设备的浏览器访问：

```text
http://<服务器IP>:8888/
```

> 手机与电脑需在同一局域网（或服务器有公网地址）。手机访问时输入服务器的局域网 IP 即可。
> 在 WSL2（NAT 模式）下手动启动时，需先在 Windows 配置端口转发（见 `run.sh` 内部实现）。

## HTTP API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | 前端页面（手机 / 电脑浏览器；默认 `~/.datahub/index.html`） |
| POST | `/api/text` | 上传文本；body 为内容 → `{"id":"XXXXXX"}` |
| GET | `/api/text/<id>` | 按提取码获取文本 |
| POST | `/api/file` | 上传文件；header `X-File-Name` 指定文件名，body 为内容 → `{"id":"XXXXXX"}` |
| GET | `/api/file/<id>` | 按提取码下载文件（响应头含 `Content-Disposition`） |
| GET | `/api/list` | 列出全部数据项（JSON 数组） |
| DELETE | `/api/item/<id>` | 删除数据项 |

### 命令行示例

```bash
# 上传文本
curl -X POST -d '你好，这是要传输的内容' http://127.0.0.1:8888/api/text

# 获取文本
curl http://127.0.0.1:8888/api/text/<提取码>

# 上传文件
curl -X POST -H 'X-File-Name: photo.jpg' --data-binary @photo.jpg \
     http://127.0.0.1:8888/api/file

# 下载文件
curl -OJ http://127.0.0.1:8888/api/file/<提取码>
```

## 目录结构

```text
DataHub/
├── main.cpp                    # 入口（解析端口、忽略 SIGPIPE）
├── run.sh                      # 一键脚本：编译 + 配置外部访问(WSL 端口转发) + 启动
├── datahub.ini                 # 配置（端口 / 日志）
├── Application/
│   └── DataHubApplication.*    # CDataHubApplication（基于 CMyApplication）
├── Module/
│   ├── IDataStore.h            # 数据存储接口（sc 命名空间）
│   ├── IHttpService.h          # HTTP 服务接口（sc 命名空间）
│   ├── DataStoreModule.*       # 内存存储模块
│   └── HttpServerModule.*      # Workflow HTTP 服务模块 + 路由
├── Web/
│   └── index.html             # 前端页面源文件（构建时部署到用户目录 ~/.datahub/）
└── Linux/
    └── Makefile                # 生成 build/datahub + 部署前端页面
```

## 说明

- 数据仅存内存，进程退出即清空（精简版；如需持久化可在 `CDataStoreModule` 扩展落盘）。
- 文件大小受 Workflow 请求体内存限制；超大文件建议分批或改流式存储。
- 服务器需监听 `0.0.0.0`（Workflow 默认），以便局域网设备访问。
- **前端页面部署**：构建 DataHub 时，Makefile 自动把 `Web/index.html` 拷贝到用户目录 `~/.datahub/index.html`（固定路径，与工作目录无关）。
- **页面路径**：运行时从 `~/.datahub/index.html` 读取；如需自定义，可在 `datahub.ini` 的 `[web] index` 填绝对路径。修改 HTML 后重启服务即可生效，无需重新编译 C++。
