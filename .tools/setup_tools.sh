#!/usr/bin/env bash
set -euo pipefail

# 在无 sudo 环境下安装 compiledb（用于生成 compile_commands.json，供 clangd 使用）
# 使用方式：bash .tools/setup_tools.sh
WORKSPACE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_DIR="$WORKSPACE_ROOT/.tools/venv"
GET_PIP="/tmp/get-pip.py"

if [[ -x "$VENV_DIR/bin/compiledb" ]]; then
    echo "compiledb 已就绪: $VENV_DIR/bin/compiledb"
    exit 0
fi

echo "创建 venv: $VENV_DIR"
python3 -m venv --without-pip "$VENV_DIR"

echo "引导 pip ..."
curl -sS -m 30 -o "$GET_PIP" https://bootstrap.pypa.io/get-pip.py
"$VENV_DIR/bin/python" "$GET_PIP" --quiet

echo "安装 compiledb ..."
"$VENV_DIR/bin/python" -m pip install --quiet compiledb

echo "compiledb 已就绪: $VENV_DIR/bin/compiledb"
