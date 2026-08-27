# ------------------------------------------------------------------
# examples 示例项目 Makefile（参考标准服务器项目结构）
#   - 项目结构：<项目>/main.cpp（入口）+ <项目>/Linux/Makefile（构建配置）
#   - 生成 build/<模式>/examples（可执行文件）
#   - 直接编译所需 Common 源码（AsyncExecutorNoThrow / ThreadPool），不依赖 libCommon.a
# 可用目标:
#   make            —— 构建 examples
#   make run        —— 构建并运行
#   make compiledb  —— 生成 examples/compile_commands.json（clangd）
#   make clean      —— 清理构建产物与编译数据库
# 覆盖编译参数（如调试构建）:
#   make CXXFLAGS="-std=c++11 -Wall -Wextra -O0 -g -pthread"
# ------------------------------------------------------------------

# 通过本 Makefile 所在位置推导项目根 / 工作区根，保证在任意工作目录下调用都正确
# 注意：变量行不能带行尾注释（# 前空格会被保留进变量值）
MKFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
# examples 项目根
ROOT       := $(abspath $(MKFILE_DIR)/..)
# 工作区根
WS_ROOT    := $(abspath $(MKFILE_DIR)/../..)
# 项目名（examples）
PROJECT    := $(notdir $(ROOT))

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O2 -g -pthread
# 构建模式（release / debug）：产物分目录 build/<模式>/（由 build.sh 传入）
BUILD_MODE ?= release
# 项目根 + Common + asio 子模块
CPPFLAGS ?= -I$(ROOT) \
            -I$(WS_ROOT)/Common \
            -I$(WS_ROOT)/Common/ThirdParty/asio/asio/include -DASIO_STANDALONE

SRC_DIR   := $(ROOT)
BUILD_DIR := $(WS_ROOT)/build/$(BUILD_MODE)
TARGET    := $(BUILD_DIR)/examples

# 示例源码（递归收集项目根下 .cpp，排除 Linux/）
SRCS := $(shell find $(SRC_DIR) -name '*.cpp' -not -path '*/Linux/*' | sort)
OBJS := $(foreach src,$(SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(src))).o)
DEPS := $(OBJS:.o=.d)

# 示例依赖的 Common 源码（直接编译，不依赖 libCommon.a；后续示例若依赖更多模块在此追加）
COMMON_SRCS := $(WS_ROOT)/Common/Async/AsyncExecutorNoThrow.cpp \
               $(WS_ROOT)/Common/Thread/ThreadPool.cpp
COMMON_OBJS := $(foreach src,$(COMMON_SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(WS_ROOT)/Common/%.cpp,%,$(src))).o)
COMMON_DEPS := $(COMMON_OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS) $(COMMON_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

# 仅编译对象文件（不链接），供编译数据库生成使用
.PHONY: objects
objects: $(OBJS) $(COMMON_OBJS)

# 示例源码编译规则
define compile-obj
$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(1))).o: $(1)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach src,$(SRCS),$(eval $(call compile-obj,$(src))))

# Common 源码编译规则（对象带 PROJECT 前缀，避免与其它项目对象冲突）
define compile-common
$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(WS_ROOT)/Common/%.cpp,%,$(1))).o: $(1)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach src,$(COMMON_SRCS),$(eval $(call compile-common,$(src))))

# 依赖文件（头文件变更时自动重编对应 .o）
-include $(DEPS) $(COMMON_DEPS)

.PHONY: run
run: $(TARGET)
	$(TARGET)

# 生成 compile_commands.json（供 clangd，compiledb 以 dry-run 捕获全部编译命令）
.PHONY: compiledb
compiledb:
	compiledb -n -o $(ROOT)/compile_commands.json make all

.PHONY: clean
clean:
	rm -rf $(OBJS) $(DEPS) $(COMMON_OBJS) $(COMMON_DEPS) $(TARGET) $(ROOT)/compile_commands.json

.PHONY: all run clean
