# ------------------------------------------------------------------
# ServerCore 静态库 Makefile
#   - 头文件位于 <项目>/Include/
#   - 源码位于 <项目>/Src/
#   - 生成 build/libServerCore.a
# 可用目标:
#   make                        —— 构建 libServerCore.a
#   make debug                  —— 调试构建（-O0）
#   make objects                —— 仅编译对象文件（不生成库）
#   make compile_commands.json  —— 使用 bear 生成编译数据库（供 clangd）
#   make compiledb              —— 使用 compiledb 生成编译数据库
#   make clean                  —— 清理构建产物与编译数据库
# ------------------------------------------------------------------

# 通过本 Makefile 所在位置推导项目根目录，保证在任意工作目录下调用都正确
MKFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT       := $(abspath $(MKFILE_DIR)/..)
WS_ROOT    := $(abspath $(MKFILE_DIR)/../..)
PROJECT    := $(notdir $(ROOT))

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O2 -g -pthread
# 本项目各模块目录 + Common 各模块目录 + asio 子模块
CPPFLAGS ?= -I$(ROOT) -I$(WS_ROOT)/Common -I$(WS_ROOT)/Common/ThirdParty/asio/asio/include -DASIO_STANDALONE
LDFLAGS  ?=
LDLIBS   ?=

SRC_DIR   := $(ROOT)
BUILD_DIR := $(WS_ROOT)/build
TARGET    := $(BUILD_DIR)/libServerCore.a

# 递归收集各模块目录下的 .cpp（排除 Linux 构建目录）
SRCS := $(shell find $(SRC_DIR) -name '*.cpp' -not -path '*/Linux/*' | sort)
# 对象名 = <项目>_<路径拍平>.o，避免不同项目同名源文件冲突
OBJS := $(foreach src,$(SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(src))).o)
DEPS := $(OBJS:.o=.d)

# 默认目标
all: $(TARGET)

# 生成静态库
$(TARGET): $(OBJS)
	ar rcs $@ $^

# 仅编译对象文件（不生成库），供编译数据库生成使用
.PHONY: objects
objects: $(OBJS)

# 为每个源文件生成对应编译规则
define compile-obj
$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(1))).o: $(1)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach src,$(SRCS),$(eval $(call compile-obj,$(src))))

# 引入自动生成的头文件依赖
-include $(DEPS)

# ------------------------------------------------------------------
# 生成 compile_commands.json（供 clangd 使用）
# ------------------------------------------------------------------

# 方式一：bear
.PHONY: compile_commands.json
compile_commands.json:
	bear --output $(ROOT)/compile_commands.json -- make clean all

# 方式二：compiledb（只编译对象，避免递归构建依赖项目污染编译数据库）
.PHONY: compiledb
compiledb:
	compiledb -n -o $(ROOT)/compile_commands.json make debug

# 调试构建（-O0 关闭优化，便于调试）
.PHONY: debug
debug:
	$(MAKE) all CXXFLAGS="-std=c++11 -Wall -Wextra -O0 -g -pthread"

clean:
	rm -rf $(OBJS) $(DEPS) $(TARGET) $(ROOT)/compile_commands.json

.PHONY: all debug run clean
