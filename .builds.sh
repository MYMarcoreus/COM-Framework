#!/usr/bin/env bash
set -euo pipefail

# 工作区统一构建脚本：按依赖顺序构建所有项目
# 使用方式：bash .builds.sh
WORKSPACE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 依赖顺序：ServerCore 必须先于依赖它的项目构建
PROJECTS=(ServerCore Demo)

for project in "${PROJECTS[@]}"; do
    PROJECT_ROOT="$WORKSPACE_ROOT/$project"
    if [[ ! -d "$PROJECT_ROOT/Linux" ]]; then
        echo "Warning: $PROJECT_ROOT/Linux not found, skipping"
        continue
    fi
    echo "==================== Building $project ===================="
    (cd "$PROJECT_ROOT/Linux" && make all)
done

echo "==================== Build finished ===================="
