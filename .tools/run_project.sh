#!/usr/bin/env bash
set -euo pipefail

# ====================================================================
# 构建并运行指定可执行项目（供 VS Code「run (select project)」下拉任务调用）
#
# 用法：.tools/run_project.sh <项目> [模式]
#   项目：可执行项目名（来自下拉 input:projectExec；可执行名 = 项目名小写）
#   模式：debug|release|--debug|--release（默认 release）
#
# 说明：
#   - 项目是否可执行由 ./build.sh --executables 判定（单一来源）。
#   - 库项目（无可执行文件）仅完成构建。
# ====================================================================

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="${1:?用法: run_project.sh <项目> [模式]}"
MODE_ARG="${2:-release}"

cd "$ROOT"

# —— 归一化构建模式 ——
case "$MODE_ARG" in
    --debug|debug)     MODE_NAME="debug";   MODE_FLAG="--debug" ;;
    --release|release) MODE_NAME="release"; MODE_FLAG="--release" ;;
    *) echo "未知构建模式: $MODE_ARG（应为 debug / release）"; exit 1 ;;
esac

# —— 按所选模式构建 ——
./build.sh "$MODE_FLAG" "$PROJ"

# —— 定位可执行文件（可执行名 = 项目名小写）——
EXE="$(echo "$PROJ" | tr '[:upper:]' '[:lower:]')"
EXE_PATH="build/$MODE_NAME/$EXE"
if [[ ! -x "$EXE_PATH" ]]; then
    echo "【run】未找到可执行文件: $EXE_PATH（$PROJ 可能是库项目，仅完成构建）"
    exit 0
fi
echo "【run】运行: $EXE_PATH"
"$EXE_PATH"
