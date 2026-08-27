# C++ 多项目工作区（Dev Container + Makefile + clangd）

一个基于 **Dev Container（仅 Dockerfile，无 compose）** 的 C++ 多项目工作区：

- 工作区根目录可存放 **多个独立项目**；
- 每个项目按模块自治组织：`<Project>/<Module>/`（每模块一个目录，头源同目录）、`<Project>/Linux/Makefile`；
- 通过 **bear** 或 **compiledb** 生成 `compile_commands.json`，供 clangd 提供补全与跳转；
- 统一使用 **C++11**，构建工具为 **Make**（不引入 CMake）。

## 目录结构

```text
COM-Framework/                  # 工作区根目录（可存放多个项目）
├── build.sh                     # 工作区统一构建脚本（构建 / 编译数据库 / 测试 / 清理）
├── .devcontainer/
│   ├── devcontainer.json        # VS Code 开发容器配置
│   └── Dockerfile               # 工具链：g++ / make / bear / compiledb / clangd
├── .clangd                      # clangd 配置
├── .gitignore
├── .tools/
│   └── setup_tools.sh           # 宿主机（无 sudo）安装 compiledb 的脚本
│
├── Common/                      # 公共基础库（静态库 libCommon.a，模块自治：头源同目录）
│   ├── Network/                 # TcpServer / TcpClient / TcpConnection / UdpSocket / Buffer（基于 asio）
│   ├── Log/                     # Logger（自实现：线程安全，控制台 + 文件，支持文件滚动）
│   ├── Timer/                   # TimerManager（基于 asio::steady_timer）
│   ├── Config/                  # 配置解析（基于 inih）
│   ├── Thread/                  # ThreadPool（自实现：多线程任务队列）
│   ├── Async/                   # AsyncExecutor（Task 链式调用：Then / Get）
│   ├── Serialization/           # 二进制序列化（CBinaryWriter / CBinaryReader，小端 + 边界检查）
│   ├── Linux/Makefile           # 生成 build/libCommon.a
│   └── ThirdParty/              # git 子模块
│       ├── asio/                # Standalone Asio（asio-1-38-2）：网络 + 定时器
│       └── inih/                # inih（r62）：INI 解析
│
├── ServerCore/                  # 服务器基础框架（静态库 libServerCore.a，模块自治）
│   ├── Application/             # MyApplication（生命周期 + 配置注入 + 运行状态 + 默认装配）
│   ├── Module/                  # 模块模型：IUnknown/InterfaceId/ScopedInterfacePtr + IModule/Module/ModuleManager + InitContext（依赖注入）
│   ├── Event/                   # IEventDispatcher / EventDispatcher（同步 + 异步事件分发）
│   ├── Message/                 # IMessageRouter / MessageRouter（消息流水线：半包/粘包/按类型分发）
│   ├── Infra/                   # ILogger/IConfig/ITimer/IThreadPool/IAsyncExecutor + *Module（模块化适配层）
│   ├── Observability/           # IMetrics / MetricsModule（统一指标：计数器/仪表 + 状态报告聚合）
│   ├── Process/                 # Process 工具 / PidFile（守护进程 + pid 文件）
│   ├── Network/                 # INetwork / INetworkHandler / NetworkModule / TcpServerModule（含连接级上下文）
│   ├── docs/                    # 架构与开发文档（多 md：模块/注入/消息/网络/事件/指标/序列化/扩展/测试）
│   └── Linux/
│       └── Makefile             # 生成 build/libServerCore.a
│
├── Demo/                        # ServerCore 验证项目（模块自治）
│   ├── demo.ini                 # 示例配置（Config 模块）
│   ├── Application/             # DemoApplication
│   ├── Protocol/                # Demo 极简协议（Length + Command + Payload）
│   ├── Service/                 # DemoService（实现 INetworkHandler）
│   ├── Module/                  # DemoLoggerModule / DemoTimerModule / DemoLogReporterModule
│   ├── Client/                  # 测试客户端入口（client_main）
│   ├── main.cpp                 # 服务器入口
│   └── Linux/
│       └── Makefile             # 生成 build/demo 与 build/demo_client
│
├── ServerA/                     # 第一个业务服务器骨架（复用 ServerCore，模块自治）
│   ├── Application/             # ServerApplication
│   ├── Service/                 # EchoService（实现 INetworkHandler）
│   ├── Module/                  # ServerLoggerModule
│   ├── main.cpp                 # 服务器入口
│   └── Linux/
│       └── Makefile             # 生成 build/servera
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
│   ├── test_common.cpp          # 测试用例
│   ├── test_servercore.cpp
│   ├── test_logserver.cpp
│   ├── test_infra.cpp
│   ├── main.cpp
│   └── Linux/Makefile           # 生成 build/tests（make run 运行全部用例）
│
├── examples/                    # 示例项目（参考标准项目结构，直接编译所需 Common 源码）
│   ├── main.cpp                 # 示例入口（AsyncExecutorNoThrow 完整示例）
│   └── Linux/
│       └── Makefile             # 生成 build/examples（cd examples/Linux && make run）
│
└── build/                       # 构建产物（.o / .a / 可执行文件）
```

## Common 基础库

`Common` 是**服务器无关的公共基础设施**（`libCommon.a`），优先复用轻量级第三方库（git 子模块），不足处自实现：

- **Network**：基于 **Standalone Asio** 的 `TcpServer`（服务端，含连接管理与统计：`ConnectionCount`/`TotalAccepted`/`PeerAddress` 等）、`TcpClient`（主动连接，支持主机名解析与对端关闭通知）、`TcpConnection`（异步读写）、`UdpSocket`（UDP 数据报收发）、`Buffer`，通过 `std::function` 回调上报事件。
- **Log**：`Logger`（自实现，C++11 标准库即可满足）——线程安全、等级过滤、控制台 + 文件。
- **Timer**：`TimerManager` 基于 **asio::steady_timer**（独立 io 线程，一次性 / 周期性）。
- **Config**：基于 **inih** 的 INI 解析（`key=value`、注释、`[section]` 分组）。
- **Thread**：`ThreadPool`（自实现，C++11 `std::thread` + `mutex` + `condition_variable`）——`Start` / `Submit` / `Stop`，固定线程数任务队列。
- **Async**：`AsyncExecutor` 基于自实现 `ThreadPool`，提供**链式调用**——`Submit` 返回 `Task<T>`，支持 `Then`（类型变换链）、`FromResult`、`Get`、`OnSuccess` / `OnFailure`，异常自动沿链传播；无返回值任务使用 `Post`。

以上模块均可被任意服务器项目复用，且不依赖 ServerCore。

## ServerCore

`ServerCore` 是其他服务器可直接复用的**基础模块库**，不包含具体业务与协议：

- **Application**：`MyApplication` 提供统一生命周期 `Initialize → Start → Run → Stop → Shutdown`，通过 `ModuleManager` 统一管理所有模块（默认装配 IConfig / ILogger）；支持配置路径注入（`SetConfigPath`）、运行状态查询（`IsRunning` / `UptimeSeconds`）、优雅关闭超时（`SetShutdownTimeout`）。
- **Component**：COM 组件模型思想的 C++11/Linux 基础设施——`IUnknown`（QueryInterface / AddRef / Release）、`InterfaceId`（接口标识）、`ScopedInterfacePtr`（RAII 智能引用）。
- **Module**：**统一的生命周期管理模型**（组件与模块统一为 Module）——`IModule`（继承 IUnknown：名字 / 生命周期 / 状态查询 `GetState` / 状态报告 `GetStatus`）、`Module`（统一实现骨架：原子引用计数 + 接口查询 + 默认生命周期）、`ModuleManager`（注册即持有引用；既可按名字注册业务模块、也可按接口标识注册服务模块（服务定位）；统一编排：按序初始化/启动、逆序停止/关闭、失败回滚；提供 `StatusReport` / `Snapshot` / `HasModule` / `GetModuleByIid` 等查询）。
- **Event**：**事件分发**机制——`IEventDispatcher`（继承 IUnknown，`Subscribe` 返回订阅标识 / `Unsubscribe` / `Publish` / `SubscriberCount`）、`EventDispatcher`（线程安全，发布在锁外调用处理器防重入死锁），用于模块之间的解耦通信。
- **Message**：**基础消息分发**机制——`IMessageRouter`（协议无关：按连接维护缓冲，通过业务提供的 `MessageExtractor` 切分完整消息，按类型路由到 `MessageHandler`），支持粘包切分、跨包重组、非法数据丢弃。
- **Infra**：**模块化适配层**——把 Common 基础设施适配为模块接口：`ILogger` / `IConfig` / `ITimer` / `IThreadPool` / `IAsyncExecutor`，通过 `ModuleManager` 按接口访问（符合规范 §10.2）。
- **Process**：**进程级基础设施**——`Process`（`Daemonize` 守护进程化 / pid 文件读写 / 存活检查）、`PidFile`（RAII，析构自动删除）。
- **Network**：轻量通信基础设施，基于 **Standalone Asio**（git 子模块）——`TcpServer`（io_context + acceptor + 独立线程事件循环）、`TcpConnection`（异步读写 + 写队列）、`Buffer`、`INetwork`/`INetworkHandler` 接口；`INetwork` 提供连接管理与统计（`ConnectionCount` / `TotalAccepted` / `PeerAddress` 等）。

依赖方向：`Demo / Server → ServerCore → Common → 第三方库 / POSIX`。ServerCore 不依赖任何具体业务项目；网络基础设施位于 Common，ServerCore 通过 `NetworkModule` 适配为组件模型接口（`INetwork` / `INetworkHandler`）。

## Demo

`Demo` 是 ServerCore 的**验证项目**，包含一个极简协议（`Length + Command + Payload`）与协议处理服务，验证完整链路：

```text
main → MyApplication → ModuleManager → Network → TCP Listen → Accept
      → Receive → Demo Protocol → Service → Response
```

- `build/demo`：服务器（端口优先级：命令行参数 > `demo.ini` 配置 > 默认 9000）
- `build/demo_client`：测试客户端（发送 PING/ECHO 并打印响应）

Demo 使用 Common 基础库：Logger 记录日志（控制台 + 可选文件）、Config 读取 `demo.ini`、TimerManager 周期性输出运行状态。

Demo 以**模块化**方式装配：`DemoApplication` 在 `RegisterComponents()` 中按接口注册网络 / 事件 / 服务模块，在 `RegisterModules()` 中注册 `DemoLoggerModule`、`DemoTimerModule`、`DemoNetworkModule` 三个业务模块，其生命周期由 `ModuleManager` 按注册顺序统一初始化/启动、逆序停止/关闭，验证 ServerCore 的模块管理机制。

Demo 还验证**事件解耦通信**：`DemoNetworkModule` 启动/停止时发布 `network.started` / `network.stopped` 事件，`DemoApplication` 订阅并记录日志，发布者与订阅者互不依赖。

## ServerA

`ServerA` 是**第一个业务服务器骨架**，演示 ServerCore 在多个服务器项目间的复用（多服务器组织）。

- `build/servera`：服务器（默认端口 9100，可用命令行参数覆盖）
- 复用 ServerCore：`MyApplication` 生命周期 + `ModuleManager` 统一装配（接口注册 / 名字注册）+ `EventDispatcher` 事件通信 + `NetworkModule` 网络
- 业务层：`EchoService`（实现 `INetworkHandler`，收到数据原样返回，验证网络收发链路）
- 不含具体业务（规范：第一阶段不做业务认证 / 权限 / 数据库等），作为后续内网安全服务控制台后端的起点

## 使用方式

### 1. 在 Dev Container 中打开

在 VS Code 中打开本工作区，选择 **“在容器中重新打开”**。

容器基于 `ubuntu:24.04`，提供 gcc/g++、make、bear、compiledb、clangd/clang、git、gdb，并以非 root 用户 `ubuntu`（uid=1000，与宿主机一致）运行，避免容器内构建产物在宿主机上出现 root 属主问题。创建完成后 `postCreateCommand` 自动执行：

```bash
./build.sh --compiledb       # 生成所有项目的 compile_commands.json（供 clangd）
./build.sh                   # 构建 Common → ServerCore → LogServer → Demo → ServerA → Tests
```

> 容器内 clangd 直接使用容器路径（`/workspace/...`）的 `compile_commands.json`，无需路径改写。

### 2. 构建与运行

统一构建入口 `./build.sh`（自动发现所有含 `Linux/Makefile` 的项目，新项目只需新建 `<名称>/Linux/Makefile` 即可自动纳入）：

```bash
./build.sh                    # 发布构建：所有项目 + examples（一键）
./build.sh --debug            # 调试构建（-O0，含 examples，供 VS Code 调试）
./build.sh --compiledb        # 生成所有项目 + examples 的 compile_commands.json（clangd）
./build.sh --tests            # 构建并运行单元测试
./build.sh --clean            # 清理所有构建产物
./build.sh --list             # 列出自动发现的项目（全部可构建）
./build.sh --executables       # 列出可执行项目（可构建 + 项目根有 main.cpp）
./build.sh Common ServerCore  # 只构建指定项目
./build.sh examples           # 只构建 examples（examples 与服务器项目同为普通项目，统一自动发现）
```

**构建产物按模式分目录**（release 与 debug 隔离，可同时存在、互不干扰）：

```text
build/release/     —— 发布产物（demo、demo_client、logserver、servera、tests、lib*.a、examples/）
build/debug/       —— 调试产物（-O0）
```

> `build.sh` 是唯一构建入口（整合了原 `build-all.sh` / `build-debug.sh` / `generate-compiledb.sh` 与根 `Makefile`）。项目级构建用各项目 `Linux/Makefile`（由 `build.sh` 调用）。

在 VS Code 中可通过任务（`Tasks: Run Build Task`）一键执行：

- **build all (release)** / **build all (debug)** —— 全量构建
- **build (select project)** —— 弹出 **VS Code 原生下拉**，先选构建模式（debug 默认 / release）再选项目（全部可构建项目，选项来自 `./build.sh --list`）
- **debug (select project)** / **run (select project)** —— 同上，但项目下拉只列出**可执行项目**（选项来自 `./build.sh --executables`：可构建 + 项目根有 `main.cpp`），分别执行 debug 构建（随后 F5 调试）/ 构建并运行（见 `.tools/run_project.sh`）
- **build debug (Common + examples)** —— VS Code 调试前的构建（`launch.json` 的 `preLaunchTask`）
- **generate compile_commands** —— 生成 compile_commands.json
- **run examples (terminal)** / **run tests (terminal)** / **run demo server (terminal)** / **run demo client (terminal)** —— 普通终端运行（release 产物）
- **clean all**

**VS Code 调试**（仅 debug 构建，在调试器中运行，支持断点 / 变量 / 调用栈）：`launch.json` 提供配置 `Debug examples (debug build)`（`build/debug/examples`）。启动方式：按 **F5** 或 **Run and Debug** 面板选择该配置（`tasks.json` 不提供调试任务，调试统一由 `launch.json` 驱动）。启动时会自动先执行 `preLaunchTask`（`build debug (Common + examples)`）完成调试构建。

运行 Demo 服务器与客户端：`./build/release/demo 9000`、`./build/release/demo_client 9000`（调试版用 `./build/debug/...`）。

### 3. 宿主机（无 sudo）生成 compile_commands.json

宿主机若未安装 `bear`/`compiledb`，先运行：

```bash
bash .tools/setup_tools.sh   # 在工作区 .tools/venv 中安装 compiledb
./build.sh --compiledb
```

`./build.sh --compiledb` 会自动优先使用系统 `compiledb`，否则回退到 `.tools/venv` 中的版本。

### 4. git 子模块（第三方库）

本项目使用 **git 管理**，第三方库以 **submodule** 引入：`Common/ThirdParty/asio`、`Common/ThirdParty/inih`。

首次克隆后需要初始化子模块：

```bash
git submodule update --init --recursive
```

上游库更新后拉取新版本（并更新父仓库记录的 commit）：

```bash
git -C Common/ThirdParty/asio fetch
# 或其它子模块：inih
git -C Common/ThirdParty/asio checkout <新标签>
git add Common/ThirdParty/asio
git commit -m "升级 asio 到 <新标签>"
```

## 常用命令

| 命令 | 说明 |
| --- | --- |
| `./build.sh` | 构建所有项目 |
| `./build.sh --compiledb` | 生成所有项目的 compile_commands.json |
| `make -C <项目>/Linux all` | 构建单个项目 |
| `make -C <项目>/Linux debug` | 调试构建（-O0） |
| `make -C <项目>/Linux compiledb` | 用 compiledb 生成编译数据库 |
| `make -C <项目>/Linux clean` | 清理构建产物与编译数据库 |
| `./build/demo <port>` | 运行 Demo 服务器 |
