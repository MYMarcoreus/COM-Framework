#!/usr/bin/env bash
set -euo pipefail

# 兼容包装：统一构建入口已迁移到 build.sh（--compiledb 生成 compile_commands.json）。
# 保留此脚本以兼容 README / skills / devcontainer 等旧引用。
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build.sh" --compiledb "$@"
