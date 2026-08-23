# ------------------------------------------------------------------
# examples 示例构建 Makefile（独立构建，不参与 libCommon.a / libServerCore.a）
#   - 示例源码位于 <项目>/examples/
#   - 生成 build/examples/<示例名>
# 可用目标:
#   make          —— 构建所有示例
#   make run      —— 构建并运行 nothrow_demo
#   make clean    —— 清理构建产物
# ------------------------------------------------------------------

MKFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT       := $(abspath $(MKFILE_DIR)/..)
BUILD_DIR  := $(ROOT)/build/examples

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O2 -g -pthread
CPPFLAGS ?= -I$(ROOT)/Common \
            -I$(ROOT)/Common/ThirdParty/asio/asio/include -DASIO_STANDALONE

# 示例依赖的 Common 源码（后续示例若依赖更多模块在此追加）
COMMON_SRCS := $(ROOT)/Common/Async/AsyncExecutorNoThrow.cpp \
               $(ROOT)/Common/Thread/ThreadPool.cpp

# 示例可执行文件
TARGET_NOTHROW := $(BUILD_DIR)/nothrow_demo

.PHONY: all run clean compiledb

all: $(TARGET_NOTHROW)

$(TARGET_NOTHROW): $(MKFILE_DIR)nothrow_demo.cpp $(COMMON_SRCS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

run: $(TARGET_NOTHROW)
	$(TARGET_NOTHROW)

# 生成 compile_commands.json（供 clangd）：先 clean 再以 dry-run 捕获全部编译命令
compiledb:
	$(MAKE) clean
	compiledb -n -o $(ROOT)/examples/compile_commands.json make all

clean:
	rm -rf $(BUILD_DIR) $(ROOT)/examples/compile_commands.json
