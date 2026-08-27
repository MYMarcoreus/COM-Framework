#!/usr/bin/env bash
set -euo pipefail

WORKSPACE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ====================================================================
# COM-Framework 统一构建脚本（构建体系唯一入口）
#
# 用法：
#   ./build.sh [选项] [项目...]
#
# 选项：
#   -d, --debug      调试构建（-O0，含 examples；供 VS Code 调试）
#   -r, --release    发布构建（-O2，默认）
#   -c, --compiledb  生成所有项目 + examples 的 compile_commands.json（clangd）
#   -t, --tests      构建并运行单元测试（./build/tests）
#   -C, --clean      清理所有构建产物
#   -l, --list        列出自动发现的项目（可构建）
#       --executables 列出可执行项目（可构建 + 项目根有 main.cpp）
#   -h, --help        显示帮助
#
#   [项目...]        只构建指定项目（如 ./build.sh Common ServerCore）
#
# 项目自动发现：任何含 Linux/Makefile 的顶层目录都会被纳入构建，
#               新项目只需新建 <名称>/Linux/Makefile 即可自动加入。
# ====================================================================

# 已知构建依赖顺序（Common 最先；新项目按依赖追加到数组末尾，或自动发现后追加）
KNOWN_ORDER=(Common ServerCore LogServer Demo ServerA Tests examples)

RELEASE_FLAGS="-std=c++11 -Wall -Wextra -O2 -g -pthread"
DEBUG_FLAGS="-std=c++11 -Wall -Wextra -O0 -g -pthread"

MODE="release"
FLAGS="$RELEASE_FLAGS"
DO_COMPILEDB=0
DO_TESTS=0
DO_CLEAN=0
DO_LIST=0
DO_EXECUTABLES=0
PROJECTS=()

usage() {
    cat <<'EOF'
COM-Framework 统一构建脚本

用法: ./build.sh [选项] [项目...]

选项:
  -d, --debug      调试构建（-O0，含 examples；供 VS Code 调试）
  -r, --release    发布构建（-O2，默认）
  -c, --compiledb  生成所有项目 + examples 的 compile_commands.json（clangd）
  -t, --tests      构建并运行单元测试（./build/tests）
  -C, --clean      清理所有构建产物
  -l, --list        列出自动发现的项目（可构建）
      --executables 列出可执行项目（可构建 + 项目根有 main.cpp）
  -h, --help        显示帮助

[项目...]          只构建指定项目（如 ./build.sh Common ServerCore）

项目自动发现：任何含 Linux/Makefile 的顶层目录都会被纳入构建，
             新项目只需新建 <名称>/Linux/Makefile 即可自动加入。
EOF
}

# 自动发现项目：含 Linux/Makefile 的顶层目录，按 KNOWN_ORDER 排序 + 追加未知
discover_projects() {
    local all
    all=$(find "$WORKSPACE_ROOT" -maxdepth 3 -type f -path "$WORKSPACE_ROOT/*/Linux/Makefile" 2>/dev/null \
        | sed "s#$WORKSPACE_ROOT/##; s#/Linux/Makefile##" | sort -u)
    local ordered=""
    local p
    for p in "${KNOWN_ORDER[@]}"; do
        if printf '%s\n' "$all" | grep -qxF "$p"; then
            ordered+=" $p"
        fi
    done
    while IFS= read -r p; do
        [[ -z "$p" ]] && continue
        if ! printf '%s\n' "${KNOWN_ORDER[@]}" | grep -qxF "$p"; then
            ordered+=" $p"
        fi
    done <<<"$all"
    echo "$ordered" | tr ' ' '\n' | sed '/^$/d'
}

# 列出可执行项目（可构建项目的子集：可构建 + 项目根有 main.cpp，不区分大小写）
list_executables() {
    discover_projects | while IFS= read -r p; do
        [[ -z "$p" ]] && continue
        if find "$WORKSPACE_ROOT/$p" -maxdepth 1 -iname 'main.cpp' -print -quit | grep -q .; then
            echo "$p"
        fi
    done
}

build_project() {
    local p=$1
    echo "==================== Building $p ($MODE) ===================="
    (cd "$WORKSPACE_ROOT/$p/Linux" && make all BUILD_MODE=$MODE CXXFLAGS="$FLAGS")
}

clean_project() {
    local p=$1 mode=$2
    echo "Cleaning $p ($mode) ..."
    (cd "$WORKSPACE_ROOT/$p/Linux" && make clean BUILD_MODE=$mode >/dev/null 2>&1) || true
}

compiledb_project() {
    local p=$1
    echo "Generating compile_commands.json for $p ..."
    (cd "$WORKSPACE_ROOT/$p/Linux" && make compiledb BUILD_MODE=$MODE)
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -d|--debug)     MODE="debug"; FLAGS="$DEBUG_FLAGS"; shift ;;
            -r|--release)   MODE="release"; FLAGS="$RELEASE_FLAGS"; shift ;;
            -c|--compiledb) DO_COMPILEDB=1; shift ;;
            -t|--tests)     DO_TESTS=1; shift ;;
            -C|--clean)     DO_CLEAN=1; shift ;;
            -l|--list)      DO_LIST=1; shift ;;
            --executables)  DO_EXECUTABLES=1; shift ;;
            -h|--help)      usage; exit 0 ;;
            -*) echo "未知选项: $1"; usage; exit 1 ;;
            *) PROJECTS+=("$1"); shift ;;
        esac
    done
}

main() {
    parse_args "$@"

    if [[ $DO_LIST -eq 1 ]]; then
        discover_projects
        exit 0
    fi

    if [[ $DO_EXECUTABLES -eq 1 ]]; then
        list_executables
        exit 0
    fi

    local projects
    if [[ ${#PROJECTS[@]} -gt 0 ]]; then
        projects=("${PROJECTS[@]}")
    else
        projects=($(discover_projects))
    fi

    if [[ $DO_CLEAN -eq 1 ]]; then
        for mode in release debug; do
            for p in "${projects[@]}"; do clean_project "$p" "$mode"; done
        done
        rm -rf "$WORKSPACE_ROOT/build/release" "$WORKSPACE_ROOT/build/debug"
        echo "==================== 已清理所有构建产物 ===================="
        exit 0
    fi

    if [[ $DO_COMPILEDB -eq 1 ]]; then
        for p in "${projects[@]}"; do compiledb_project "$p"; done
        echo "==================== compile_commands.json 生成完毕 ===================="
        exit 0
    fi

    # release / debug 产物分目录（build/<模式>/），互不干扰，无需切换时清理
    for p in "${projects[@]}"; do
        build_project "$p"
    done

    if [[ $DO_TESTS -eq 1 ]]; then
        echo "==================== 运行单元测试 ===================="
        "$WORKSPACE_ROOT/build/$MODE/tests"
    fi

    echo "==================== Build finished ===================="
}

main "$@"
