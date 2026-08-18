# C++ 多项目工作区（Dev Container + Makefile + clangd）

一个基于 **Dev Container（仅 Dockerfile，无 compose）** 的 C++ 多项目工作区：

- 工作区根目录可存放 **多个独立项目**；
- 每个项目按规范组织：`<Project>/Include/`（头文件）、`<Project>/Src/`（源文件）、`<Project>/Linux/Makefile`；
- 通过 **bear** 或 **compiledb** 生成 `compile_commands.json`，供 clangd 提供补全与跳转；
- 统一使用 **C++11**，构建工具为 **Make**（不引入 CMake）。

## 目录结构

```text
make_test/                       # 工作区根目录（可存放多个项目）
├── .builds.sh                   # 工作区统一构建脚本（按依赖顺序构建所有项目）
├── build.sh                     # 生成所有项目的 compile_commands.json（供 clangd）
├── .devcontainer/
│   ├── devcontainer.json        # VS Code 开发容器配置
│   └── Dockerfile               # 工具链：g++ / make / bear / compiledb / clangd
├── .clangd                      # clangd 配置
├── .gitignore
├── .tools/
│   └── setup_tools.sh           # 宿主机（无 sudo）安装 compiledb 的脚本
│
├── Common/
│   └── ThirdParty/
│       └── asio/                # git 子模块：Standalone Asio（asio-1-38-2）
│
├── Common/                      # 公共基础库（静态库 libCommon.a）
│   ├── Include/
│   │   ├── Network/             # TcpServer / TcpConnection / Buffer（基于 asio）
│   │   ├── Log/                 # Logger（线程安全，控制台 + 文件）
│   │   ├── Thread/              # ThreadPool
│   │   ├── Timer/               # TimerManager（一次性 / 周期性）
│   │   ├── Config/              # INI 风格配置解析
│   │   └── Async/               # AsyncExecutor（std::future 结果）
│   ├── Src/
│   ├── Linux/Makefile           # 生成 build/libCommon.a
│   └── ThirdParty/asio/         # git 子模块：Standalone Asio（asio-1-38-2）
│
├── ServerCore/                  # 服务器基础框架（静态库 libServerCore.a）
│   ├── Include/
│   │   ├── Application/         # MyApplication
│   │   ├── Component/           # IUnknown / Component / ComponentManager / ScopedInterfacePtr
│   │   ├── Network/             # INetwork / INetworkHandler / NetworkComponent（组件模型适配）
│   │   └── Common/              # 类型别名
│   ├── Src/
│   └── Linux/
│       └── Makefile             # 生成 build/libServerCore.a
│
├── Demo/                        # ServerCore 验证项目
│   ├── demo.ini                 # 示例配置（Config 模块）
│   ├── Include/
│   │   ├── Protocol/            # Demo 极简协议（Length + Command + Payload）
│   │   ├── Service/             # DemoService（实现 INetworkHandler）
│   │   └── DemoApplication.h
│   ├── Src/                     # DemoApplication / main / client_main
│   └── Linux/
│       └── Makefile             # 生成 build/demo 与 build/demo_client
│
└── build/                       # 构建产物（.o / .a / 可执行文件）
```

## Common 基础库

`Common` 是**服务器无关的公共基础设施**（`libCommon.a`），包含：

- **Network**：基于 **Standalone Asio**（git 子模块）的 `TcpServer` / `TcpConnection` / `Buffer`，通过 `std::function` 回调上报事件，不依赖具体业务接口。
- **Log**：线程安全的 `Logger`，等级过滤 + 时间戳，输出到控制台并可同时写入文件。
- **Thread**：`ThreadPool` 线程池。
- **Timer**：`TimerManager` 定时器管理器（一次性 / 周期性，独立线程）。
- **Config**：INI 风格配置解析（`key=value`、注释、`[section]` 分组）。
- **Async**：`AsyncExecutor` 异步执行器（基于线程池 + `std::future`）。

以上模块均可被任意服务器项目复用，且不依赖 ServerCore。

## ServerCore

`ServerCore` 是其他服务器可直接复用的**基础组件库**，不包含具体业务与协议：

- **Application**：`MyApplication` 提供统一生命周期 `Initialize → Start → Run → Stop → Shutdown`，通过 `ComponentManager` 组合基础组件。
- **Component**：借鉴 COM 组件模型思想的 C++11/Linux 实现——`IUnknown`（QueryInterface / AddRef / Release）、`Component`（原子引用计数）、`ComponentManager`（注册/获取/移除/清空）、`ScopedInterfacePtr`（RAII 智能引用）。
- **Network**：轻量通信基础设施，基于 **Standalone Asio**（git 子模块）——`TcpServer`（io_context + acceptor + 独立线程事件循环）、`TcpConnection`（异步读写 + 写队列）、`Buffer`、`INetwork`/`INetworkHandler` 接口。

依赖方向：`Demo / Server → ServerCore → Common → 第三方库 / POSIX`。ServerCore 不依赖任何具体业务项目；网络基础设施位于 Common，ServerCore 通过 `NetworkComponent` 适配为组件模型接口（`INetwork` / `INetworkHandler`）。

## Demo

`Demo` 是 ServerCore 的**验证项目**，包含一个极简协议（`Length + Command + Payload`）与协议处理服务，验证完整链路：

```text
main → MyApplication → ComponentManager → Network → TCP Listen → Accept
      → Receive → Demo Protocol → Service → Response
```

- `build/demo`：服务器（端口优先级：命令行参数 > `demo.ini` 配置 > 默认 9000）
- `build/demo_client`：测试客户端（发送 PING/ECHO 并打印响应）

Demo 使用 Common 基础库：Logger 记录日志（控制台 + 可选文件）、Config 读取 `demo.ini`、TimerManager 周期性输出运行状态。

## 使用方式

### 1. 在 Dev Container 中打开

在 VS Code 中打开本工作区，选择 **“在容器中重新打开”**。

容器基于 `ubuntu:24.04`，提供 gcc/g++、make、bear、compiledb、clangd/clang、git、gdb，并以非 root 用户 `ubuntu`（uid=1000，与宿主机一致）运行，避免容器内构建产物在宿主机上出现 root 属主问题。创建完成后 `postCreateCommand` 自动执行：

```bash
bash build.sh   # 生成所有项目的 compile_commands.json（供 clangd）
bash .builds.sh # 构建 Common → ServerCore → Demo
```

> 容器内 clangd 直接使用容器路径（`/workspace/...`）的 `compile_commands.json`，无需路径改写。

### 2. 构建与运行

```bash
# 工作区统一构建（按依赖顺序：Common → ServerCore → Demo）
bash .builds.sh

# 生成 compile_commands.json（供 clangd）
bash build.sh

# 单独构建某个项目
make -C ServerCore/Linux all
make -C Demo/Linux all

# 运行 Demo 服务器与客户端
./build/demo 9000
./build/demo_client 9000
```

也可以在 VS Code 中运行任务（`Tasks: Run Build Task`）：

- **build (all projects)** —— `.builds.sh`
- **generate compile_commands (all projects)** —— `build.sh`
- **run demo server** / **run demo client**

### 3. 宿主机（无 sudo）生成 compile_commands.json

宿主机若未安装 `bear`/`compiledb`，先运行：

```bash
bash .tools/setup_tools.sh   # 在工作区 .tools/venv 中安装 compiledb
bash build.sh
```

`build.sh` 会自动优先使用系统 `compiledb`，否则回退到 `.tools/venv` 中的版本。

### 4. git 子模块（第三方库）

本项目使用 **git 管理**，第三方库以 **submodule** 引入（当前为 `Common/ThirdParty/asio`）。

首次克隆后需要初始化子模块：

```bash
git submodule update --init --recursive
```

上游库更新后拉取新版本（并更新父仓库记录的 commit）：

```bash
git -C Common/ThirdParty/asio fetch
git -C Common/ThirdParty/asio checkout <新标签>
git add Common/ThirdParty/asio
git commit -m "升级 asio 到 <新标签>"
```

## 常用命令

| 命令 | 说明 |
| --- | --- |
| `bash .builds.sh` | 构建所有项目 |
| `bash build.sh` | 生成所有项目的 compile_commands.json |
| `make -C <项目>/Linux all` | 构建单个项目 |
| `make -C <项目>/Linux debug` | 调试构建（-O0） |
| `make -C <项目>/Linux compiledb` | 用 compiledb 生成编译数据库 |
| `make -C <项目>/Linux clean` | 清理构建产物与编译数据库 |
| `./build/demo <port>` | 运行 Demo 服务器 |
