# ------------------------------------------------------------------
# examples 示例构建 Makefile（独立构建，不参与 libCommon.a / libServerCore.a）
#   - 示例源码位于 <项目>/examples/
#   - 生成 build/examples/<示例名>，支持增量编译（.o 缓存 + .d 依赖）
# 可用目标:
#   make            —— 构建所有示例
#   make run        —— 构建并运行 nothrow_demo
#   make compiledb  —— 生成 examples/compile_commands.json（clangd）
#   make clean      —— 清理构建产物
# 覆盖编译参数（如调试构建）:
#   make CXXFLAGS="-std=c++11 -Wall -Wextra -O0 -g -pthread"
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
COMMON_OBJS := $(COMMON_SRCS:$(ROOT)/Common/%.cpp=$(BUILD_DIR)/Common/%.o)

# 示例源码（按需追加：如 nothrow_demo.cpp、other_demo.cpp ...）
# abspath 保证绝对路径（make -C 下 MKFILE_DIR 是相对的）
EXAMPLE_SRCS := $(abspath $(MKFILE_DIR)nothrow_demo.cpp)
EXAMPLE_OBJS := $(EXAMPLE_SRCS:$(ROOT)/examples/%.cpp=$(BUILD_DIR)/%.o)

OBJS := $(COMMON_OBJS) $(EXAMPLE_OBJS)
DEPS := $(OBJS:.o=.d)

# 示例可执行文件
TARGET_NOTHROW := $(BUILD_DIR)/nothrow_demo

.PHONY: all run clean compiledb

all: $(TARGET_NOTHROW)

$(TARGET_NOTHROW): $(EXAMPLE_OBJS) $(COMMON_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Common 源码编译规则（输出到 build/examples/Common/）
$(BUILD_DIR)/Common/%.o: $(ROOT)/Common/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

# examples 源码编译规则（输出到 build/examples/）
$(BUILD_DIR)/%.o: $(ROOT)/examples/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

# 依赖文件（头文件变更时自动重编对应 .o）
-include $(DEPS)

run: $(TARGET_NOTHROW)
	$(TARGET_NOTHROW)

# 生成 compile_commands.json（供 clangd）：先 clean 再以 dry-run 捕获全部编译命令
compiledb:
	$(MAKE) clean
	compiledb -n -o $(ROOT)/examples/compile_commands.json make all

clean:
	rm -rf $(BUILD_DIR) $(ROOT)/examples/compile_commands.json
