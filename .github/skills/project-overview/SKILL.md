---

name: project-overview
description: 本项目的总体架构和开发环境规范。涉及项目结构、服务器项目组织、开发环境、工作区结构、项目间关系或整体架构设计时使用。
-----------------------------------------------------------------------

# 项目总体规范

## 1. 项目定位

本项目是一个**通用 COM 模块化服务器框架**。

项目采用多服务器、多项目的组织方式，不是一个单一的 C++ 工程。

不同服务器负责不同的业务或基础设施功能，共享公共基础组件和 `ServerCore`。

总体目标：

* 建立统一的服务器运行基础。
* 提供可复用的服务器基础设施。
* 保持服务器项目之间清晰的依赖关系。
* 降低第三方库依赖。
* 保持构建环境可复现。
* 保持代码兼容 C++11。
* 避免过度设计。

---

## 2. 开发环境

项目运行和开发环境：

* Windows 主机
* WSL
* Ubuntu 24.04
* VS Code
* Docker
* VS Code Dev Container

开发容器由：

```text
.devcontainer/devcontainer.json
Dockerfile
```

定义。

**Ubuntu 24.04 Dev Container 是项目的标准开发和构建环境。**

不要默认使用 Windows 主机环境编译项目。

遇到编译、依赖或者工具链问题时，优先检查：

1. `devcontainer.json`
2. `Dockerfile`
3. Makefile
4. `build.sh`
5. 编译器版本
6. 依赖库

---

## 3. 工作区结构

工作区可以包含多个独立项目。

典型结构：

```text
Workspace/
├── build.sh
├── .devcontainer/
│   ├── devcontainer.json
│   └── Dockerfile
│
├── Common/
│
├── ServerCore/
│   ├── Module/        # 模块模型
│   ├── Network/
│   ├── Event/
│   ├── ...
│   └── Linux/
│       └── Makefile
│
├── ServerA/
│   ├── Application/
│   ├── Module/
│   ├── Service/
│   └── Linux/
│       └── Makefile
│
├── ServerB/
│   ├── ...
│   └── Linux/
│       └── Makefile
│
└── ...
```

---

## 4. 标准项目结构

所有 C++ 项目统一采用**模块自治**组织：每个模块一个目录，头文件（.h）与源文件（.cpp）同目录；可执行项目的入口与配置放在项目根。

```text
<ProjectName>/
├── <Module>/    # 领域模块目录（如 Module/Network/Service/Protocol/Application），头源同目录
├── main.cpp     # 可执行项目入口（库项目无）
├── <name>.ini   # 配置文件（可选）
└── Linux/
    └── Makefile
```

- **模块目录**：每个模块一个目录，内含该模块全部 `.h` 与 `.cpp`；命名遵循 name-standard（接口 `I*`、实现 `*Module`/`*Service` 等）。Common 的 `ThirdParty/`（asio/inih）除外。
- **入口与配置**：`main.cpp` 与 `.ini` 位于项目根；客户端入口可放 `Client/` 子目录。
- **Linux**：存放 Makefile；Makefile 用 `find` 递归收集各模块目录下的 `.cpp`（排除 `Linux/`，Common 还需排除 `ThirdParty/`）。

不要把业务源码放入 `Linux/`。

---

## 5. 项目依赖关系

推荐依赖方向：

```text
业务服务器
    ↓
ServerCore
    ↓
Common / Infrastructure
    ↓
第三方库 / POSIX / 系统库
```

禁止形成循环依赖。

例如禁止：

```text
ServerCore → ServerA
ServerA → ServerCore
```

`ServerCore` 不允许依赖具体业务服务器。

---

## 6. 开发原则

遵循以下优先级：

```text
已有项目规范
    ↓
已有基础组件
    ↓
C++11 标准库
    ↓
Linux/POSIX
    ↓
轻量级第三方库
    ↓
大型框架
```

优先采用简单、可靠、容易维护的方案。

不要因为某个框架功能强大，就为简单需求引入大型框架。

---

## 7. 修改已有代码

修改代码前必须先理解：

* 项目结构
* 调用关系
* 依赖关系
* 对象生命周期
* 线程关系
* 构建方式

优先进行最小修改。

不要为了实现一个小功能而重构整个模块。

不要创建已经存在功能的重复实现。

---

## 8. 新增项目

创建新项目时：

```text
<ProjectName>/
├── <Module>/    # 领域模块目录（头源同目录）
├── main.cpp     # 可执行入口（库项目无）
└── Linux/
    └── Makefile
```

然后：

1. 明确项目职责。
2. 明确依赖的公共组件。
3. 确定是否依赖 `ServerCore`。
4. 创建 Makefile。
5. 接入工作区构建流程。
6. 确保 clangd 可以正常工作。
7. 在 Dev Container 中完成编译验证。

---

## 9. 不允许的默认行为

除非明确要求，否则：

* 不要引入 CMake。
* 不要改成 C++17。
* 不要把项目改造成单体工程。
* 不要引入大型服务器框架。
* 不要随意改变目录结构。
* 不要修改 Dev Container 架构。
* 不要复制第三方源码到多个项目。
* 不要把业务代码放入 `ServerCore`。
* 不要为了消除 clangd 报错而关闭 clangd。
