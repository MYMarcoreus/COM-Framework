#!/usr/bin/env bash
set -euo pipefail

# 工作区编译数据库生成脚本：为所有项目生成 compile_commands.json（供 clangd）
# 使用方式：bash generate-compiledb.sh
# 编译逻辑集中在各项目 Makefile 的 compiledb 目标中，本脚本只负责按顺序调用。
PROJECTS=(Common ServerCore LogServer Demo ServerA Tests)

WORKSPACE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 优先使用系统 compiledb（Dev Container 中已安装），
# 否则使用工作区 .tools/venv 中的 compiledb（宿主机无 sudo 场景）。
if command -v compiledb >/dev/null 2>&1; then
    COMPILEDB_DIR="$(dirname "$(command -v compiledb)")"
elif [[ -x "$WORKSPACE_ROOT/.tools/venv/bin/compiledb" ]]; then
    COMPILEDB_DIR="$WORKSPACE_ROOT/.tools/venv/bin"
else
    echo "Error: compiledb 未安装。请先运行 bash .tools/setup_tools.sh 或安装 compiledb。" >&2
    exit 1
fi

export PATH="$COMPILEDB_DIR:$PATH"

for project in "${PROJECTS[@]}"; do
    PROJECT_ROOT="$WORKSPACE_ROOT/$project"
    if [[ ! -d "$PROJECT_ROOT/Linux" ]]; then
        echo "Warning: $PROJECT_ROOT/Linux not found, skipping"
        continue
    fi
    echo "Generating compile_commands.json for $project ..."
    (cd "$PROJECT_ROOT/Linux" && make compiledb)
    echo "Generated: $PROJECT_ROOT/compile_commands.json"
done

echo "==================== compile_commands.json 生成完毕 ===================="
