# VS Code 原生下拉框（Tasks Shell Input）接入指南

> 在 VS Code 的「运行任务」中，通过**原生下拉（Quick Pick）**依次选择**构建模式**和**项目**，然后自动执行构建 / 调试 / 运行。
> 本文记录从零到可用的完整流程，可在其他电脑或其他项目复刻。

---

## 1. 效果预览

运行 `Tasks: Run Task`，选择某个 `(select project)` 任务后：

1. 先弹出**构建模式**下拉：`debug（默认）` / `release`
2. 再弹出**项目**下拉：全部可构建项目（build）或可执行项目（debug/run）
3. 选定后自动执行对应命令

```
Tasks: Run Task
 └─ build (select project)
     ├─ ① 下拉选构建模式：debug（默认）/ release
     └─ ② 下拉选项目（自动发现）
```

## 2. 原理

VS Code 原生 `inputs` 的 `pickString` 只支持**静态选项**，无法自动发现项目。通过安装 **Tasks Shell Input** 扩展，把输入类型改为 `command` + `shellCommand.execute`，其 `args.command` 的**每行输出**即成为一个下拉选项：

- 全部可构建项目 → `./build.sh --list`（含 `Linux/Makefile` 的顶层目录）
- 可执行项目 → `./build.sh --executables`（可构建 + 项目根有 `main.cpp`，不区分大小写）

## 3. 前置条件

- VS Code（稳定版）
- Linux / WSL（以下基于 bash）
- 项目统一构建脚本 `build.sh`，且约定「可构建项目 = 顶层存在 `<项目>/Linux/Makefile`」

## 4. 安装扩展

在扩展市场搜索安装 **Tasks Shell Input**：

- 名称：Tasks Shell Input
- ID：`augustocdias.tasks-shell-input`

安装后必须执行 **Reload Window**（命令面板 → `Developer: Reload Window`）才会生效。

## 5. 需要的文件

### 5.1 构建脚本 `build.sh`：提供项目列表命令

给 `build.sh` 增加两个「只输出项目列表」的选项，供下拉插件调用。核心是两个函数 + 两个参数：

```bash
# 自动发现项目：含 Linux/Makefile 的顶层目录（可按已知顺序排序，此处从简）
discover_projects() {
    local all
    all=$(find "$WORKSPACE_ROOT" -maxdepth 3 -type f -path "$WORKSPACE_ROOT/*/Linux/Makefile" 2>/dev/null \
        | sed "s#$WORKSPACE_ROOT/##; s#/Linux/Makefile##" | sort -u)
    echo "$all"
}

# 可执行项目：可构建 + 项目根有 main.cpp（-iname 不区分大小写）
list_executables() {
    discover_projects | while IFS= read -r p; do
        [[ -z "$p" ]] && continue
        if find "$WORKSPACE_ROOT/$p" -maxdepth 1 -iname 'main.cpp' -print -quit | grep -q .; then
            echo "$p"
        fi
    done
}
```

参数解析中注册 `--list` 与 `--executables`：

```bash
-l|--list)      DO_LIST=1; shift ;;
--executables)  DO_EXECUTABLES=1; shift ;;
```

主流程中提前处理并退出：

```bash
if [[ $DO_LIST -eq 1 ]]; then discover_projects; exit 0; fi
if [[ $DO_EXECUTABLES -eq 1 ]]; then list_executables; exit 0; fi
```

> **要点**：这两个命令必须**只输出项目名（每行一个）**，不要打印任何日志/横幅，否则会污染下拉选项。

### 5.2 运行脚本 `.tools/run_project.sh`（仅 `run` 需要）

`run` 需要把「模式 → 路径」并定位可执行文件，放一个精简脚本（无交互）：

```bash
#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="${1:?用法: run_project.sh <项目> [模式]}"
MODE_ARG="${2:-release}"
cd "$ROOT"

# 归一化构建模式
case "$MODE_ARG" in
    --debug|debug)     MODE_NAME="debug";   MODE_FLAG="--debug" ;;
    --release|release) MODE_NAME="release"; MODE_FLAG="--release" ;;
    *) echo "未知构建模式: $MODE_ARG"; exit 1 ;;
esac

./build.sh "$MODE_FLAG" "$PROJ"

# 可执行名约定 = 项目名小写（Demo→demo、examples→examples）
EXE="$(echo "$PROJ" | tr '[:upper:]' '[:lower:]')"
EXE_PATH="build/$MODE_NAME/$EXE"
if [[ ! -x "$EXE_PATH" ]]; then
    echo "未找到可执行文件: $EXE_PATH（可能是库项目，仅完成构建）"
    exit 0
fi
"$EXE_PATH"
```

创建后赋予执行权限：`chmod +x .tools/run_project.sh`

### 5.3 `.vscode/tasks.json`：下拉接入（核心）

完整内容如下（3 个下拉任务 + 2 个必要支撑任务 + 3 个输入）：

```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "build (select project)",
            "type": "shell",
            "command": "./build.sh ${input:mode} ${input:project}",
            "dependsOn": ["generate compile_commands"],
            "problemMatcher": ["$gcc"],
            "presentation": { "reveal": "always", "panel": "shared" }
        },
        {
            "label": "debug (select project)",
            "type": "shell",
            "command": "./build.sh --debug ${input:projectExec}",
            "dependsOn": ["generate compile_commands"],
            "problemMatcher": ["$gcc"],
            "presentation": { "reveal": "always", "panel": "shared" }
        },
        {
            "label": "run (select project)",
            "type": "shell",
            "command": "bash .tools/run_project.sh ${input:projectExec} ${input:mode}",
            "problemMatcher": [],
            "presentation": { "reveal": "always", "panel": "dedicated" }
        },
        {
            "label": "build debug (Common + ServerCore)",
            "type": "shell",
            "command": "./build.sh --debug Common examples",
            "dependsOn": ["generate compile_commands"],
            "problemMatcher": ["$gcc"],
            "presentation": { "reveal": "always", "panel": "shared" }
        },
        {
            "label": "generate compile_commands",
            "type": "shell",
            "command": "./build.sh --compiledb",
            "problemMatcher": [],
            "presentation": { "reveal": "always", "panel": "shared" }
        }
    ],
    "inputs": [
        {
            "id": "mode",
            "type": "pickString",
            "description": "选择构建模式",
            "options": [
                { "label": "debug（默认）", "value": "--debug" },
                { "label": "release", "value": "--release" }
            ],
            "default": "--debug"
        },
        {
            "id": "project",
            "type": "command",
            "command": "shellCommand.execute",
            "args": {
                "command": "./build.sh --list",
                "cwd": "${workspaceFolder}",
                "description": "选择项目（全部可构建项目，可手动输入）",
                "allowCustomValues": true
            }
        },
        {
            "id": "projectExec",
            "type": "command",
            "command": "shellCommand.execute",
            "args": {
                "command": "./build.sh --executables",
                "cwd": "${workspaceFolder}",
                "description": "选择可执行项目（可构建 + 有 main.cpp，可手动输入）",
                "allowCustomValues": true
            }
        }
    ]
}
```

**关键点**：

- `mode`：`pickString`，`options` 的 `value` 直接取 `--debug`/`--release`（与 `build.sh` 参数一致），`default` 必须是其中一个 `value`。
- `project` / `projectExec`：`"type": "command"` + `"command": "shellCommand.execute"`；`args.command` 是**纯输出列表**的命令；`args.cwd` 设 `${workspaceFolder}` 保证相对路径正确；`allowCustomValues: true` 允许手动输入（不受列表限制）。
- `build debug (Common + ServerCore)`：若被 `launch.json` 的 `preLaunchTask` 引用则**必须保留**，否则 F5 调试报错。
- `generate compile_commands`：各 select 任务的 `dependsOn`（生成 clangd 编译数据库）；不需要可移除，并去掉各任务的 `dependsOn`。

### 5.4 `launch.json`（可选，F5 调试用）

```jsonc
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Debug examples (debug build)",
            "type": "lldb",                 // 或 cppdbg 等，取决于调试扩展
            "request": "launch",
            "program": "${workspaceFolder}/build/debug/examples",
            "cwd": "${workspaceFolder}",
            "preLaunchTask": "build debug (Common + ServerCore)"   // 启动前自动构建
        }
    ]
}
```

## 6. 从零复刻步骤

1. 安装扩展 **Tasks Shell Input**（`augustocdias.tasks-shell-input`），执行 Reload Window。
2. 在 `build.sh` 增加 `--list` / `--executables` 两个纯列表选项（见 5.1），并验证：
   ```bash
   ./build.sh --list          # 每行一个可构建项目名
   ./build.sh --executables   # 只含可执行项目
   ```
3. 新建 `.tools/run_project.sh` 并 `chmod +x`（可选，仅 `run` 需要）。
4. 把 `.vscode/tasks.json` 配置为 5.3 的内容，按实际项目调整 `build debug` 任务的项目名与命令路径。
5. （可选）配置 `launch.json`，`preLaunchTask` 指向 tasks.json 中的构建任务。
6. Reload Window 后，`Tasks: Run Task` → 选任一 `(select project)` → 依次弹出模式、项目下拉。
7. 运行 `./build.sh --compiledb` 生成各项目的 `compile_commands.json`（供 clangd）。

## 7. 验证清单

- [ ] 扩展已安装并 Reload Window
- [ ] `./build.sh --list` 只输出项目名（无日志）
- [ ] `./build.sh --executables` 只含可执行项目（不含库项目）
- [ ] `Tasks: Run Task` → `build (select project)` 先弹「模式」下拉（debug 默认）
- [ ] 选定模式后弹「项目」下拉（可构建列表）
- [ ] 选定后命令执行成功
- [ ] `debug (select project)` 的下拉只列可执行项目
- [ ] `run (select project)` 能构建并运行可执行文件

## 8. 常见问题

- **下拉为空 / 只有报错文本**：`args.command` 必须只输出项目名；检查 `./build.sh --list` 是否有额外日志或报错输出（stderr 默认会告警，可用 `args.stdio: "stdout"` 忽略）。
- **完全不弹下拉**：扩展未安装，或未 Reload Window。
- **相对路径不对**：`args.cwd` 务必设 `${workspaceFolder}`，`args.command` 用相对路径（`./build.sh ...`）。
- **想一次选多个项目**：`args` 增加 `"multiselect": true` 与 `"multiselectSeparator": " "`。
- **可执行名规则**：本文约定「可执行名 = 项目名小写」，与 `build/<模式>/<小写名>` 对应；若项目不满足该约定，需调整 `run_project.sh` 的定位逻辑。

---

## 附：本项目文件清单

| 文件 | 说明 |
|---|---|
| `build.sh` | 统一构建脚本，含 `--list` / `--executables` |
| `.tools/run_project.sh` | run 执行器（构建 + 定位可执行文件） |
| `.vscode/tasks.json` | 下拉任务与 inputs 定义 |
| `.vscode/launch.json` | F5 调试配置（可选） |
