# 构建与使用指南

> 本页为详细操作指南（Dev Container / 构建 / 运行 / 调试 / 子模块）。
> 工作区概览见 [README](../README.md)，完整文档索引见 [README.md](README.md)。

## 1. 在 Dev Container 中打开

在 VS Code 中打开本工作区，选择 **“在容器中重新打开”**。

容器基于 `ubuntu:24.04`，提供 gcc/g++、make、bear、compiledb、clangd/clang、git、gdb，并以非 root 用户 `ubuntu`（uid=1000，与宿主机一致）运行，避免容器内构建产物在宿主机上出现 root 属主问题。创建完成后 `postCreateCommand` 自动执行：

```bash
./build.sh --compiledb       # 生成所有项目的 compile_commands.json（供 clangd）
./build.sh                   # 构建 Common → ServerCore → LogServer → Demo → ServerA → Tests
```

> 容器内 clangd 直接使用容器路径（`/workspace/...`）的 `compile_commands.json`，无需路径改写。

## 2. 构建与运行

统一构建入口 `./build.sh`（自动发现所有含 `Linux/Makefile` 的项目，新项目只需新建 `<名称>/Linux/Makefile` 即可自动纳入）：

```bash
./build.sh                    # 发布构建：所有项目 + examples（一键）
./build.sh --debug            # 调试构建（-O0，含 examples，供 VS Code 调试）
./build.sh --compiledb        # 生成所有项目 + examples 的 compile_commands.json（clangd）
./build.sh --tests            # 构建并运行单元测试
./build.sh --clean            # 清理所有构建产物
./build.sh --list             # 列出自动发现的项目（全部可构建）
./build.sh --executables      # 列出可执行项目（可构建 + 项目根有 main.cpp）
./build.sh Common ServerCore  # 只构建指定项目
./build.sh examples           # 只构建 examples（examples 与服务器项目同为普通项目，统一自动发现）
```

**构建产物按模式分目录**（release 与 debug 隔离，可同时存在、互不干扰）：

```text
build/release/     —— 发布产物（demo、demo_client、logserver、servera、tests、lib*.a、examples/）
build/debug/       —— 调试产物（-O0）
```

> `build.sh` 是唯一构建入口（整合了原 `build-all.sh` / `build-debug.sh` / `generate-compiledb.sh` 与根 `Makefile`）。项目级构建用各项目 `Linux/Makefile`（由 `build.sh` 调用）。

### VS Code 任务

在 VS Code 中可通过任务（`Tasks: Run Task`）执行（select 由原生下拉驱动；完整的接入与跨机器复刻指南见 [vscode-select-dropdown.md](vscode-select-dropdown.md)）：

- **build (select project)** —— 先选构建模式（debug 默认 / release）再选项目（全部可构建项目，选项来自 `./build.sh --list`）
- **debug (select project)** / **run (select project)** —— 项目下拉只列出**可执行项目**（选项来自 `./build.sh --executables`：可构建 + 项目根有 `main.cpp`），分别执行 debug 构建（随后 F5 调试）/ 构建并运行（见 `.tools/run_project.sh`）
- **build debug (Common + examples)** —— VS Code 调试前的构建（`launch.json` 的 `preLaunchTask`）
- **generate compile_commands** —— 生成 compile_commands.json（供 clangd）

> 全量构建 / 清理等批量操作不再提供任务，直接使用 `./build.sh`（全量）与 `./build.sh --clean`（清理）。

### VS Code 调试

仅 debug 构建，在调试器中运行，支持断点 / 变量 / 调用栈：`launch.json` 提供配置 `Debug examples (debug build)`（`build/debug/examples`）。启动方式：按 **F5** 或 **Run and Debug** 面板选择该配置（`tasks.json` 不提供调试任务，调试统一由 `launch.json` 驱动）。启动时会自动先执行 `preLaunchTask`（`build debug (Common + examples)`）完成调试构建。

运行 Demo 服务器与客户端：`./build/release/demo 9000`、`./build/release/demo_client 9000`（调试版用 `./build/debug/...`）。

## 3. 宿主机（无 sudo）生成 compile_commands.json

宿主机若未安装 `bear`/`compiledb`，先运行：

```bash
bash .tools/setup_tools.sh   # 在工作区 .tools/venv 中安装 compiledb
./build.sh --compiledb
```

`./build.sh --compiledb` 会自动优先使用系统 `compiledb`，否则回退到 `.tools/venv` 中的版本。

## 4. git 子模块（第三方库）

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
