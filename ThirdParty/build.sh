#!/usr/bin/env bash
# ====================================================================
# ThirdParty 第三方库独立编译脚本
#
# 定位：第三方库不纳入主构建（build.sh），本脚本单独编译其静态库，
#       产出到 build/<模式>/lib<名称>.a，供各项目 Makefile 链接时直接使用。
#
# 用法：
#   ./ThirdParty/build.sh            —— 编译全部第三方库（release）
#   ./ThirdParty/build.sh --debug    —— 调试构建（-O0）
#   ./ThirdParty/build.sh workflow   —— 只编译指定库（asio/inih/workflow）
#   ./ThirdParty/build.sh --clean    —— 清理构建产物
#
# 当前支持：
#   asio     —— header-only，无需编译（仅提供头文件路径）
#   inih     —— 编译 libinih.a
#   workflow —— 编译 libworkflow.a（nossl 分支，无 OpenSSL 依赖）
# ====================================================================
set -euo pipefail

WS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MODE="release"
DO_CLEAN=0
TARGETS=()

RELEASE_CXXFLAGS="-std=c++11 -Wall -O2 -g"
DEBUG_CXXFLAGS="-std=c++11 -Wall -O0 -g"
RELEASE_CFLAGS="-Wall -std=gnu90 -O2 -g"
DEBUG_CFLAGS="-Wall -std=gnu90 -O0 -g"

# workflow 官方强制标志（-fno-exceptions 等），不可被外部覆盖
WORKFLOW_CXXFLAGS="-fPIC -pipe -fno-exceptions -Wno-invalid-offsetof"
WORKFLOW_CFLAGS="-fPIC -pipe"

usage() {
    cat <<'EOF'
ThirdParty 第三方库独立编译脚本

用法: ./ThirdParty/build.sh [选项] [库...]

选项:
  --debug    调试构建（-O0，默认 release）
  --clean    清理构建产物
  -h, --help 显示帮助

[库...]      只编译指定库（asio / inih / workflow），默认全部
EOF
}

# 编译 workflow（nossl）——纯 Make，不引入 CMake
build_workflow() {
    local build_dir="$WS_ROOT/build/$MODE"
    local src_dir="$TP_ROOT/workflow/src"
    local out="$build_dir/libworkflow.a"
    echo "==> Compiling workflow -> $out"

    # 源码未拉取时提示
    if [[ ! -d "$src_dir" ]]; then
        echo "!! workflow 子模块未拉取，请先执行: git submodule update --init --recursive"
        return 1
    fi

    local cxxflags="$1" cflags="$2"
    # 收集源码（排除 Kafka：需外部库 snappy/zstd/lz4，默认不启用）
    local objs=()
    local src obj
    while IFS= read -r src; do
        case "$src" in
            *WFKafkaClient.cc|*KafkaTaskImpl.cc|*kafka_parser.c|*KafkaMessage.cc|*KafkaDataTypes.cc|*KafkaResult.cc)
                continue ;;
        esac
        obj="$build_dir/workflow_$(echo "${src#$src_dir/}" | tr '/' '_')"
        objs+=("$obj")
        if [[ "$src" == *.c ]]; then
            gcc $cflags $WORKFLOW_CFLAGS -I"$src_dir/include/workflow" -MMD -MP -c "$src" -o "$obj"
        else
            g++ $cxxflags $WORKFLOW_CXXFLAGS -I"$src_dir/include/workflow" -MMD -MP -c "$src" -o "$obj"
        fi
    done < <(find "$src_dir" \( -name '*.cc' -o -name '*.c' \) | sort)

    ar rcs "$out" "${objs[@]}"
}

# 编译 inih
build_inih() {
    local build_dir="$WS_ROOT/build/$MODE"
    local src="$TP_ROOT/inih/ini.c"
    local obj="$build_dir/inih_ini.o"
    local out="$build_dir/libinih.a"
    echo "==> Compiling inih -> $out"
    if [[ ! -f "$src" ]]; then
        echo "!! inih 子模块未拉取"
        return 1
    fi
    gcc -std=c11 -Wall -O2 -g -c "$src" -o "$obj"
    ar rcs "$out" "$obj"
}

# asio 为 header-only，仅提示头文件位置
build_asio() {
    echo "==> asio: header-only，无需编译"
    echo "    头文件: $TP_ROOT/asio/asio/include"
}

main() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --debug) MODE="debug"; shift ;;
            --clean) DO_CLEAN=1; shift ;;
            -h|--help) usage; exit 0 ;;
            -*) echo "未知选项: $1"; usage; exit 1 ;;
            *) TARGETS+=("$1"); shift ;;
        esac
    done

    if [[ $DO_CLEAN -eq 1 ]]; then
        rm -f "$WS_ROOT"/build/*/libworkflow.a "$WS_ROOT"/build/*/libinih.a \
              "$WS_ROOT"/build/*/workflow_*.o "$WS_ROOT"/build/*/inih_*.o
        echo "已清理 ThirdParty 构建产物"
        exit 0
    fi

    if [[ ${#TARGETS[@]} -eq 0 ]]; then
        TARGETS=(asio inih workflow)
    fi

    local cxxflags="$RELEASE_CXXFLAGS" cflags="$RELEASE_CFLAGS"
    [[ $MODE == "debug" ]] && { cxxflags="$DEBUG_CXXFLAGS"; cflags="$DEBUG_CFLAGS"; }

    mkdir -p "$WS_ROOT/build/$MODE"
    for t in "${TARGETS[@]}"; do
        case "$t" in
            asio)     build_asio ;;
            inih)     build_inih "$cxxflags" "$cflags" ;;
            workflow) build_workflow "$cxxflags" "$cflags" ;;
            *) echo "!! 未知库: $t"; exit 1 ;;
        esac
    done

    echo "==================== ThirdParty Build finished ($MODE) ===================="
}

main "$@"
