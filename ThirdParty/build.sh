#!/usr/bin/env bash
# ====================================================================
# ThirdParty 第三方库独立编译脚本
#
# 定位：第三方库不纳入主构建（build.sh），本脚本单独编译其静态库，
#       产出到 build/<模式>/lib<名称>.a，供各项目 Makefile 链接时直接使用。
#
# 用法：
#   ./ThirdParty/build.sh            —— 编译全部第三方库（默认 debug + release 双模式）
#   ./ThirdParty/build.sh --debug    —— 仅调试构建（-O0）
#   ./ThirdParty/build.sh --release  —— 仅发布构建（-O2）
#   ./ThirdParty/build.sh workflow   —— 只编译指定库（asio/inih/workflow）
#   ./ThirdParty/build.sh --compiledb —— 生成 workflow 源码 compile_commands.json（clangd）
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

MODE=""       # 空 = 双模式（debug+release）；--debug/--release 指定单一模式
DO_CLEAN=0
DO_COMPILEDB=0
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
  --debug     仅调试构建（-O0）
  --release   仅发布构建（-O2，默认与 debug 同时构建）
  --compiledb 生成 workflow 源码的 compile_commands.json（供 clangd）
  --clean     清理构建产物
  -h, --help  显示帮助

[库...]       只编译指定库（asio / inih / workflow），默认全部
EOF
}

# 编译 workflow（nossl）——纯 Make，不引入 CMake
# 参数: <mode> <cxxflags> <cflags>
build_workflow() {
    local mode="$1"
    local cxxflags="$2" cflags="$3"
    local build_dir="$WS_ROOT/build/$mode"
    local src_dir="$TP_ROOT/workflow/src"
    local out="$build_dir/libworkflow.a"
    echo "==> [$mode] Compiling workflow -> $out"

    # 源码未拉取时提示
    if [[ ! -d "$src_dir" ]]; then
        echo "!! workflow 子模块未拉取，请先执行: git submodule update --init --recursive"
        return 1
    fi

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
# 参数: <mode> <cxxflags>
build_inih() {
    local mode="$1"
    local cxxflags="$2"
    local build_dir="$WS_ROOT/build/$mode"
    local src="$TP_ROOT/inih/ini.c"
    local obj="$build_dir/inih_ini.o"
    local out="$build_dir/libinih.a"
    echo "==> [$mode] Compiling inih -> $out"
    if [[ ! -f "$src" ]]; then
        echo "!! inih 子模块未拉取"
        return 1
    fi
    gcc $cxxflags -c "$src" -o "$obj"
    ar rcs "$out" "$obj"
}

# asio 为 header-only，仅提示头文件位置
build_asio() {
    echo "==> asio: header-only，无需编译"
    echo "    头文件: $TP_ROOT/asio/asio/include"
}

# 生成 workflow 源码的 compile_commands.json（供 clangd 解析第三方源码）
# 方案：生成临时 Makefile 描述 workflow 各源文件的编译命令，
#       用 `compiledb make --dry-run` 解析出编译数据库（不产生真实对象文件）。
compiledb_workflow() {
    local src_dir="$TP_ROOT/workflow/src"
    local out="$TP_ROOT/workflow/compile_commands.json"
    echo "==> Generating workflow compile_commands.json"

    if [[ ! -d "$src_dir" ]]; then
        echo "!! workflow 子模块未拉取"
        return 1
    fi
    if ! command -v compiledb >/dev/null 2>&1; then
        echo "!! compiledb 未安装，无法生成 compile_commands.json"
        return 1
    fi

    local tmp_dir="$WS_ROOT/build/compiledb"
    mkdir -p "$tmp_dir"

    # 临时 Makefile：描述 workflow 全部源文件的编译命令（排除 Kafka）
    local mf="$tmp_dir/Makefile.workflow"
    cat > "$mf" <<EOF
WF_SRC := $src_dir
WF_INC := \$(WF_SRC)/include/workflow
CXXFLAGS := -std=c++11 -Wall -O2 -g $WORKFLOW_CXXFLAGS
CFLAGS := -Wall -std=gnu90 -O2 -g $WORKFLOW_CFLAGS
SRCS := \$(shell find \$(WF_SRC) \\( -name '*.cc' -o -name '*.c' \\) | grep -vE 'Kafka|kafka')
OBJS := \$(SRCS:.cc=.o)
OBJS := \$(OBJS:.c=.o)
all: \$(OBJS)
%.o: %.cc
	g++ \$(CXXFLAGS) -I\$(WF_INC) -c \$< -o \$@
%.o: %.c
	gcc \$(CFLAGS) -I\$(WF_INC) -c \$< -o \$@
EOF

    # dry-run 解析编译数据库（不实际编译）
    (cd "$tmp_dir" && compiledb -n -f -o "$out" make --dry-run -f Makefile.workflow)
    echo "==> workflow compile_commands.json 已生成: $out"
}

main() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --debug)   MODE="debug"; shift ;;
            --release) MODE="release"; shift ;;
            --clean) DO_CLEAN=1; shift ;;
            --compiledb) DO_COMPILEDB=1; shift ;;
            -h|--help) usage; exit 0 ;;
            -*) echo "未知选项: $1"; usage; exit 1 ;;
            *) TARGETS+=("$1"); shift ;;
        esac
    done

    if [[ $DO_COMPILEDB -eq 1 ]]; then
        compiledb_workflow
        exit 0
    fi

    if [[ $DO_CLEAN -eq 1 ]]; then
        rm -f "$WS_ROOT"/build/*/libworkflow.a "$WS_ROOT"/build/*/libinih.a \
              "$WS_ROOT"/build/*/workflow_*.o "$WS_ROOT"/build/*/inih_*.o
        echo "已清理 ThirdParty 构建产物"
        exit 0
    fi

    if [[ ${#TARGETS[@]} -eq 0 ]]; then
        TARGETS=(asio inih workflow)
    fi

    # 默认双模式（debug + release）；--debug/--release 指定单一模式
    local modes=()
    if [[ -n "$MODE" ]]; then
        modes=("$MODE")
    else
        modes=(release debug)
    fi

    for mode in "${modes[@]}"; do
        local cxxflags="$RELEASE_CXXFLAGS" cflags="$RELEASE_CFLAGS"
        [[ $mode == "debug" ]] && { cxxflags="$DEBUG_CXXFLAGS"; cflags="$DEBUG_CFLAGS"; }
        mkdir -p "$WS_ROOT/build/$mode"
        for t in "${TARGETS[@]}"; do
            case "$t" in
                asio)     build_asio ;;
                inih)     build_inih "$mode" "$cxxflags" ;;
                workflow) build_workflow "$mode" "$cxxflags" "$cflags" ;;
                *) echo "!! 未知库: $t"; exit 1 ;;
            esac
        done
    done

    echo "==================== ThirdParty Build finished (${modes[*]}) ===================="
}

main "$@"
