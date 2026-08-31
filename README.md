# COM-Framework —— C++ 模块化服务器框架

基于 **COM 风格模块模型** 的通用 C++ 多服务器后端框架（C++11 / Make / clangd）。

这是一个**多服务器、多项目**的通用框架：多个业务服务器共享同一套基础组件与
`ServerCore` 基础设施，各自负责不同的业务功能，通过模块化方式组织与装配。

## 核心特性

- **COM 风格模块模型**：`IUnknown` / `InterfaceId` / 引用计数 + `CModuleManager`（依赖拓扑排序 / 失败回滚）+ 依赖注入
- **统一生命周期**：`MyApplication` 提供 `Initialize → Start → Run → Stop → Shutdown` 标准流程
- **可复用 ServerCore**：模块 / 事件 / 消息流水线 / 网络 / 指标 / 并发调度等基础设施，业务服务器只写协议与业务
- **并发调度（Exec）**：模块级读写并发控制 + 业务流程回调栈
- **公共基础库（Common）**：日志 / 配置 / 网络 / 定时器 / 线程池 / 异步 / 序列化
- **C++11 统一约束**，Make 构建，第三方库以 git 子模块管理（`ThirdParty/`）

> 文档索引：[docs/README.md](docs/README.md) ｜ 构建与使用：[docs/usage.md](docs/usage.md) ｜ 总体架构：[docs/architecture.md](docs/architecture.md)

## 目录结构

```text
COM-Framework/                  # 工作区根目录（可存放多个项目）
├── build.sh                     # 工作区统一构建脚本（构建 / 编译数据库 / 测试 / 清理）
├── docs/                        # 文档总览（按组件拆分 使用/实现，见 docs/README.md）
├── .clangd                      # clangd 配置
├── .gitignore
├── .tools/
│   └── setup_tools.sh           # 宿主机（无 sudo）安装 compiledb 的脚本
│
├── ThirdParty/                  # 第三方库（git 子模块，独立编译，链接时使用）
│   ├── build.sh                 # 第三方库独立编译脚本（asio / inih / workflow）
│   ├── asio/                    # Standalone Asio（asio-1-38-2）：网络 + 定时器
│   ├── inih/                    # inih（r62）：INI 解析
│   └── workflow/                # Sogou Workflow（nossl 分支）：HTTP/Redis/MySQL + 任务流
│
├── Common/                      # 公共基础库（静态库 libCommon.a，模块自治：头源同目录）
│   ├── Network/                 # TcpServer / TcpClient / TcpConnection / UdpSocket / Buffer（基于 asio）
│   ├── Log/                     # Logger（自实现：线程安全，控制台 + 文件，支持文件滚动）
│   ├── Timer/                   # TimerManager（基于 asio::steady_timer）
│   ├── Config/                  # 配置解析（基于 inih）
│   ├── Thread/                  # ThreadPool（自实现：多线程任务队列）
│   ├── Async/                   # AsyncExecutor（Task 链式调用：Then / Get）
│   ├── Serialization/           # 二进制序列化（CBinaryWriter / CBinaryReader，小端 + 边界检查）
│   ├── Storage/                 # CFileStore（通用文件存储：纯内存、线程安全、短码生成，供任意服务器复用）
│   └── Linux/Makefile           # 生成 build/libCommon.a
│
├── WorkflowDemo/                # Sogou Workflow 使用示例（server / client / parallel / graph）
│
├── DataHub/                     # 设备间 HTTP 数据传输服务（ServerCore 骨架 + Workflow HTTP）
│   ├── datahub.ini              # 示例配置（监听端口）
│   ├── Application/             # DataHubApplication
│   ├── Module/                  # DataStoreModule（内存存储）/ HttpServerModule（WFHttpServer 封装）
│   ├── Web/                     # 前端页面（独立资源 index.html，运行时从磁盘加载）
│   ├── main.cpp                 # 服务器入口
│   └── Linux/
│       └── Makefile             # 生成 build/datahub
│
├── ServerCore/                  # 服务器基础框架（静态库 libServerCore.a，模块自治）
│   ├── Application/             # MyApplication（生命周期 + 配置注入 + 运行状态 + 默认装配）
│   ├── Module/                  # 模块模型：IUnknown/InterfaceId/ScopedInterfacePtr + IModule/Module/ModuleManager + CResolveContext（依赖注入）
│   ├── Event/                   # IEventDispatcher / EventDispatcher（同步 + 异步事件分发）
│   ├── Message/                 # IMessageRouter / MessageRouter（消息流水线：半包/粘包/按类型分发）
│   ├── Network/                 # INetwork / INetworkHandler / NetworkModule / TcpServerModule（含连接级上下文）
│   ├── Infra/                   # ILogger/IConfig/ITimer/IThreadPool/IAsyncExecutor + *Module（模块化适配层）
│   ├── Observability/           # IMetrics / MetricsModule（统一指标：计数器/仪表 + 状态报告聚合）
│   ├── Process/                 # Process 工具 / PidFile（守护进程 + pid 文件）
│   ├── Exec/                    # 并发调度：全局调度器 + 模块级读写调度 + 业务流程回调栈
│   └── Linux/
│       └── Makefile             # 生成 build/libServerCore.a
│
├── ServerExample/                # ServerCore 全功能验证（模块自治）
│   ├── example.ini              # 示例配置（Config 模块）
│   ├── Application/             # ExampleApplication
│   ├── Protocol/                # Example 极简协议（Length + Command + Payload）
│   ├── Service/                 # ExampleService（实现 INetworkHandler）
│   ├── Module/                  # ExampleLoggerModule / ExampleTimerModule / ExampleLogReporterModule
│   ├── Client/                  # 测试客户端入口（client_main）
│   ├── main.cpp                 # 服务器入口
│   └── Linux/
│       └── Makefile             # 生成 build/example 与 build/example_client
│
├── ServerTemplate/              # 最小业务服务器骨架（复用 ServerCore，模块自治）
│   ├── Application/             # TemplateApplication
│   ├── Service/                 # EchoService（实现 INetworkHandler）
│   ├── Module/                  # TemplateLoggerModule
│   ├── main.cpp                 # 服务器入口
│   └── Linux/
│       └── Makefile             # 生成 build/servertemplate
│
├── LogServer/                   # 集中式日志服务器（复用 ServerCore，模块自治）
│   ├── logserver.ini            # 示例配置（监听端口 / 日志目录）
│   ├── Application/             # LogServerApplication（含 CConfigReloadModule 配置热加载广播）
│   ├── Protocol/                # LogProtocol（Length + Command + 文本负载）
│   ├── Service/                 # LogService / LogStorage（按来源分文件 + 滚动）
│   ├── Module/                  # 各业务模块
│   ├── main.cpp                 # 服务器入口
│   └── Linux/
│       └── Makefile             # 生成 build/logserver
│
├── Tests/                       # 单元测试（轻量框架，链接 Common + ServerCore + LogServer 被测源码）
│   ├── TestFramework.h/.cpp     # TEST / ASSERT_TRUE / ASSERT_EQ 宏
│   ├── test_common.cpp / test_exec.cpp / test_servercore.cpp
│   ├── test_logserver.cpp / test_infra.cpp
│   ├── main.cpp
│   └── Linux/Makefile           # 生成 build/tests（make run 运行全部用例）
│
├── examples/                    # 示例项目（参考标准项目结构，直接编译所需 Common 源码）
│   ├── main.cpp                 # 示例入口（AsyncExecutor 完整示例）
│   └── Linux/
│       └── Makefile             # 生成 build/examples（cd examples/Linux && make run）
│
└── build/                       # 构建产物（.o / .a / 可执行文件）
```

## 项目概览

### Common（公共基础库 `libCommon.a`）

服务器无关的基础库，均可被任意服务器项目复用，且不依赖 ServerCore：

- **Network**：TcpServer / TcpClient / TcpConnection / UdpSocket / Buffer（基于 Standalone Asio）
- **Log**：Logger（线程安全、等级过滤、控制台 + 文件）
- **Timer**：TimerManager（基于 asio::steady_timer，一次性 / 周期性）
- **Config**：基于 inih 的 INI 解析
- **Thread**：ThreadPool（自实现多线程任务队列）
- **Async**：AsyncExecutor（`Submit` → `Then` → `Get` 链式调用，Option 风格）
- **Serialization**：CBinaryWriter / CBinaryReader（小端 + 边界检查）

### ServerCore（服务器基础框架 `libServerCore.a`）

可直接复用的基础模块库，不包含具体业务与协议：

- **Application**：`MyApplication` 统一生命周期（Initialize → Start → Run → Stop → Shutdown）
- **Module**：模块模型（IUnknown / 引用计数 / 接口查询）+ `CModuleManager`（拓扑排序 / 失败回滚）+ 依赖注入
- **Event / Message / Network / Infra / Observability / Process / Exec**：事件分发、消息流水线、网络层、适配层、指标、进程工具、并发调度

依赖方向：`ServerExample / ServerTemplate / LogServer → ServerCore → Common → 第三方库 / POSIX`。

### 业务服务器

- **ServerExample**：ServerCore 全功能验证（极简协议 + 回显 + 事件解耦 + 模块化装配 + 日志上报）
- **ServerTemplate**：最小业务服务器骨架（`EchoService` 验证网络收发链路），可作为新服务器模板
- **LogServer**：集中式日志服务器（按来源分文件 + 滚动 + 配置热加载）

### Tests / examples

- **Tests**：单元测试（轻量框架 `TEST` + `ASSERT_TRUE`/`ASSERT_EQ`，覆盖 Common / ServerCore / Exec / LogServer）
- **examples**：示例项目（AsyncExecutor 完整示例）

## 文档

| 文档 | 内容 |
|---|---|
| [docs/README.md](docs/README.md) | 完整文档索引（按组件拆分 使用 / 实现） |
| [docs/architecture.md](docs/architecture.md) | 总体架构与分层设计 |
| [docs/usage.md](docs/usage.md) | 构建与使用指南（构建 / 运行 / 调试 / 子模块） |

## 快速开始

```bash
git submodule update --init --recursive   # 首次克隆后初始化第三方库（asio / inih / workflow）
./build.sh --compiledb                    # 生成 compile_commands.json（供 clangd）
./build.sh                                # 构建全部项目
./build/debug/example 9000                 # 运行 ServerExample 服务器（默认 debug 构建）
./build/debug/example_client 9000          # 运行测试客户端（PING→PONG）
```

详细操作（VS Code 任务 / 调试 / 宿主机 compiledb 等）见 [docs/usage.md](docs/usage.md)。
