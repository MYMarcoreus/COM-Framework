#!/usr/bin/env bash
set -euo pipefail

# 兼容包装：统一构建入口已迁移到 build.sh（--debug 调试构建）。
# 保留此脚本以兼容旧引用。
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build.sh" --debug "$@"
