#!/usr/bin/env bash
set -euo pipefail

# ====================================================================
# 交互式项目/模式选择器（供 VS Code 任务调用）
#
#   自动发现项目（复用 ./build.sh --list），支持选择构建模式：
#     - 未指定模式：集成终端 select 菜单选择（默认 debug）
#     - 显式指定模式（$3：debug|release 或 --debug|--release）
#   执行：
#     build —— ./build.sh <模式> <项目>        （按所选模式构建）
#     debug —— ./build.sh --debug <项目>       （固定 debug 构建；随后按 F5 调试）
#     run   —— 按所选模式构建并运行可执行项目
#
# 用法：.tools/project_menu.sh [build|debug|run] [项目] [模式]
#
# 说明：
#   - 项目列表完全自动发现（任何含 Linux/Makefile 的顶层目录）。
#   - 参数：1=操作，2=项目名（可选，来自原生下拉输入；省略则菜单选择），
#           3=构建模式 debug/release（可选；省略则菜单选择，默认 debug）。
#   - debug 无法在终端任务里直接启动 VS Code 调试器，只负责构建 debug 版，
#     实际调试请按 F5（见 .vscode/launch.json）。
#   - 新增可执行项目时，在 executable_of() 中补充「项目 → 可执行文件」映射。
# ====================================================================

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ACTION="${1:-build}"
PROJ="${2:-}"       # 可选：项目名（VS Code 原生下拉输入时传入，跳过项目菜单）
MODE_ARG="${3:-}"   # 可选：构建模式 debug|release|--debug|--release

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

# —— 交互式选择（read 实现；空输入用默认序号；EOF 视为取消）——
# 用法：pick <默认序号> <提示> <选项...>；结果写入 $PICK
pick() {
    local def="$1" prompt="$2"; shift 2
    local items=("$@") i n
    while true; do
        for i in "${!items[@]}"; do
            printf '  %d) %s\n' "$((i+1))" "${items[$i]}"
        done
        if ! read -r -p "$prompt" n; then
            echo "已取消"
            exit 0
        fi
        [[ -z "$n" ]] && n="$def"
        if [[ "$n" =~ ^[0-9]+$ ]] && (( n >= 1 && n <= ${#items[@]} )); then
            PICK="${items[$((n-1))]}"
            return 0
        fi
        echo "无效选择，请重试。"
    done
}

# —— 构建模式归一化：MODE_NAME=debug/release，MODE_FLAG=--debug/--release ——
MODE_NAME=""
MODE_FLAG=""
if [[ "$ACTION" == "debug" ]]; then
    # 调试固定 debug 构建
    MODE_NAME="debug"; MODE_FLAG="--debug"
else
    case "$MODE_ARG" in
        --debug|debug)     MODE_NAME="debug";   MODE_FLAG="--debug" ;;
        --release|release) MODE_NAME="release"; MODE_FLAG="--release" ;;
        "")
            # 未指定：弹出模式菜单（直接回车默认 debug）
            pick "1" "选择构建模式（直接回车默认 debug）> " "debug" "release"
            MODE_NAME="$PICK"
            if [[ "$MODE_NAME" == "debug" ]]; then MODE_FLAG="--debug"; else MODE_FLAG="--release"; fi
            ;;
        *)
            echo "未知构建模式: $MODE_ARG（应为 debug / release）"
            exit 1
            ;;
    esac
    echo "构建模式: $MODE_NAME"
fi

# —— 自动发现项目（复用 build.sh --list）——
mapfile -t PROJECTS < <(./build.sh --list)
if [[ ${#PROJECTS[@]} -eq 0 ]]; then
    echo "未发现任何项目（顶层缺少 <项目>/Linux/Makefile）"
    exit 1
fi

if [[ -z "$PROJ" ]]; then
    # —— 交互式菜单（未显式指定项目时）——
    echo "=================================================================="
    echo "  $ACTION（$MODE_NAME）—— 请选择项目"
    echo "=================================================================="
    pick "" "选择项目（输入编号，Ctrl+C 取消）> " "${PROJECTS[@]}"
    PROJ="$PICK"
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
        ./build.sh "$MODE_FLAG" "$PROJ"
        ;;
    debug)
        ./build.sh --debug "$PROJ"
        echo
        echo "【debug】$PROJ 调试构建完成。按 F5 启动调试（选择 launch.json 对应配置）。"
        ;;
    run)
        ./build.sh "$MODE_FLAG" "$PROJ"
        EXE="$(executable_of "$PROJ")"
        if [[ -z "$EXE" ]]; then
            echo "【run】$PROJ 是库项目（无可执行文件），仅完成构建。"
            exit 0
        fi
        EXE_PATH="build/$MODE_NAME/$EXE"
        if [[ ! -x "$EXE_PATH" ]]; then
            echo "【run】未找到可执行文件: $EXE_PATH（请确认已构建对应模式）"
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
