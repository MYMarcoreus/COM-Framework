#!/usr/bin/env bash
set -euo pipefail

# ====================================================================
# 交互式项目选择器（供 VS Code 任务调用）
#
#   自动发现项目（复用 ./build.sh --list）：
#     - 未指定项目：在集成终端弹出 select 菜单选择（零扩展兑底）
#     - 显式指定项目（$2，供 VS Code 原生下拉输入调用）：直接使用
#   执行：
#     build —— ./build.sh <项目>            （release 构建）
#     debug —— ./build.sh --debug <项目>    （debug 构建；随后按 F5 调试）
#     run   —— 构建并运行所选可执行项目
#
# 用法：.tools/project_menu.sh [build|debug|run] [项目]
#
# 说明：
#   - 项目列表完全自动发现（任何含 Linux/Makefile 的顶层目录）。
#   - 第一个参数是操作；第二个参数是可选的项目名（来自原生下拉输入时传入）。
#   - debug 无法在终端任务里直接启动 VS Code 调试器，只负责构建 debug 版，
#     实际调试请按 F5（见 .vscode/launch.json）。
#   - 新增可执行项目时，在 executable_of() 中补充「项目 → 可执行文件」映射。
# ====================================================================

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ACTION="${1:-build}"
PROJ="${2:-}"   # 可选：显式指定项目（VS Code 原生下拉输入时传入，跳过菜单）

cd "$ROOT"

# —— 项目 → 可执行文件（相对 build/<mode>/）；空表示纯库项目（无可执行文件）——
executable_of() {
    case "$1" in
        Demo)      echo "demo" ;;
        ServerA)   echo "servera" ;;
        LogServer) echo "logserver" ;;
        Tests)     echo "tests" ;;
        examples)  echo "examples/examples" ;;
        *)         echo "" ;;   # Common / ServerCore 等静态库
    esac
}

# —— 自动发现项目（复用 build.sh --list）——
mapfile -t PROJECTS < <(./build.sh --list)
if [[ ${#PROJECTS[@]} -eq 0 ]]; then
    echo "未发现任何项目（顶层缺少 <项目>/Linux/Makefile）"
    exit 1
fi

if [[ -z "$PROJ" ]]; then
    # —— 交互式菜单（未显式指定项目时）——
    echo "=================================================================="
    echo "  $ACTION —— 请选择项目"
    echo "=================================================================="
    PS3="输入编号选择项目（Ctrl+C 取消）> "
    select PROJ in "${PROJECTS[@]}"; do
        if [[ -n "$PROJ" ]]; then
            break
        fi
        echo "无效选择，请重试。"
    done
    echo "已选择: $PROJ"
    echo
else
    # —— 显式指定（VS Code 原生下拉输入）——
    if ! printf '%s\n' "${PROJECTS[@]}" | grep -qxF "$PROJ"; then
        echo "【警告】$PROJ 不在自动发现列表中，将尝试直接处理。"
    fi
fi

case "$ACTION" in
    build)
        ./build.sh "$PROJ"
        ;;
    debug)
        ./build.sh --debug "$PROJ"
        echo
        echo "【debug】$PROJ 调试构建完成。按 F5 启动调试（选择 launch.json 对应配置）。"
        ;;
    run)
        ./build.sh "$PROJ"
        EXE="$(executable_of "$PROJ")"
        if [[ -z "$EXE" ]]; then
            echo "【run】$PROJ 是库项目（无可执行文件），仅完成构建。"
            exit 0
        fi
        EXE_PATH="build/release/$EXE"
        if [[ ! -x "$EXE_PATH" ]]; then
            echo "【run】未找到可执行文件: $EXE_PATH（请确认已构建 release 版）"
            exit 1
        fi
        echo "【run】运行: $EXE_PATH"
        "$EXE_PATH"
        ;;
    *)
        echo "未知操作: $ACTION（应为 build / debug / run）"
        exit 1
        ;;
esac
