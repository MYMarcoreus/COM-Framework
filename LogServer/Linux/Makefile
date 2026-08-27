# ------------------------------------------------------------------
# LogServer 日志服务器 Makefile（ServerCore 复用骨架）
#   - 模块自治：每模块一个目录，头源同目录
#   - 生成 build/logserver
#   - 依赖 build/libServerCore.a 与 build/libCommon.a
# 可用目标:
#   make                        —— 构建 logserver
#   make debug                  —— 调试构建（-O0）
#   make objects                —— 仅编译本项目对象文件（不依赖 ServerCore）
#   make debug_objects          —— 调试参数编译本项目对象文件
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
# 构建模式（release / debug）：产物分目录 build/<模式>/（由 build.sh 传入）
BUILD_MODE ?= release
# 本项目各模块目录 + ServerCore + Common + asio 子模块
CPPFLAGS ?= -I$(ROOT) -I$(WS_ROOT)/ServerCore -I$(WS_ROOT)/Common -I$(WS_ROOT)/Common/ThirdParty/asio/asio/include -DASIO_STANDALONE
LDFLAGS  ?=
LDLIBS   ?= $(BUILD_DIR)/libServerCore.a $(BUILD_DIR)/libCommon.a

SRC_DIR   := $(ROOT)
BUILD_DIR := $(WS_ROOT)/build/$(BUILD_MODE)
TARGET    := $(BUILD_DIR)/logserver
LIB       := $(BUILD_DIR)/libServerCore.a
COMMON_LIB := $(BUILD_DIR)/libCommon.a

# 递归收集各模块目录下的 .cpp（排除 Linux）
SRCS := $(shell find $(SRC_DIR) -name '*.cpp' -not -path '*/Linux/*' | sort)
OBJS := $(foreach src,$(SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(src))).o)
DEPS := $(OBJS:.o=.d)

# 默认目标
all: $(TARGET)

# 链接 ServerCore 与 Common 静态库
$(TARGET): $(OBJS) $(LIB) $(COMMON_LIB)
	$(CXX) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

# 仅编译本项目对象文件（不依赖 ServerCore 库），供编译数据库生成使用
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

# 依赖的 ServerCore / Common 静态库：缺失时自动构建
$(LIB):
	$(MAKE) -C $(WS_ROOT)/ServerCore/Linux all

$(COMMON_LIB):
	$(MAKE) -C $(WS_ROOT)/Common/Linux all

# ------------------------------------------------------------------
# 生成 compile_commands.json（供 clangd 使用）
# ------------------------------------------------------------------

# 方式一：bear
.PHONY: compile_commands.json
compile_commands.json:
	bear --output $(ROOT)/compile_commands.json -- make clean objects

# 方式二：compiledb（只编译本项目对象，避免递归构建依赖项目污染编译数据库）
.PHONY: compiledb
compiledb:
	compiledb -n -o $(ROOT)/compile_commands.json make debug_objects

# 调试构建（-O0 关闭优化，便于调试）
.PHONY: debug
debug:
	$(MAKE) all CXXFLAGS="-std=c++11 -Wall -Wextra -O0 -g -pthread"

# 调试参数编译本项目对象文件
.PHONY: debug_objects
debug_objects:
	$(MAKE) objects CXXFLAGS="-std=c++11 -Wall -Wextra -O0 -g -pthread"

# 运行服务器
.PHONY: run
run: $(TARGET)
	$(TARGET) $(PORT)

clean:
	rm -rf $(OBJS) $(DEPS) $(TARGET) $(ROOT)/compile_commands.json

.PHONY: all debug run clean
