#!/usr/bin/env bash
set -euo pipefail

# 调试构建脚本：所有项目 + examples 以 -O0 构建。
#
# 用途：
#   - 供 VS Code 调试（NOTHROW_LOC 等调试信息在 -O0 下生效）；
#   - 一键统一调试构建（等价于 make debug）。
#
# 注意：
#   - 会覆盖 build/ 下的发布产物；想恢复发布版请运行 bash build-all.sh 或 make。
#   - 先 clean 再构建：make 不感知 CXXFLAGS 变化，必须重建才能切换到 -O0。
WORKSPACE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 依赖顺序：Common → ServerCore → LogServer → Demo → ServerA → Tests
PROJECTS=(Common ServerCore LogServer Demo ServerA Tests)

for project in "${PROJECTS[@]}"; do
    PROJECT_ROOT="$WORKSPACE_ROOT/$project"
    if [[ ! -d "$PROJECT_ROOT/Linux" ]]; then
        echo "Warning: $PROJECT_ROOT/Linux not found, skipping"
        continue
    fi
    echo "==================== Building $project (debug) ===================="
    # 统一用 CXXFLAGS="-O0" 覆盖（不依赖各项目是否定义 debug target）：
    # 先 clean 再构建——make 不感知 CXXFLAGS 变化，必须重建才能切换到 -O0。
    (cd "$PROJECT_ROOT/Linux" && make clean >/dev/null 2>&1 && \
        make all CXXFLAGS="-std=c++11 -Wall -Wextra -O0 -g -pthread")
done

echo "==================== Building examples (debug) ===================="
(cd "$WORKSPACE_ROOT/examples" && make clean >/dev/null 2>&1 && \
    make CXXFLAGS="-std=c++11 -Wall -Wextra -O0 -g -pthread")

echo "==================== Build (debug) finished ===================="
