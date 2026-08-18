# ------------------------------------------------------------------
# Common 基础库 Makefile（日志 / 定时器 / 线程 / 网络 / 配置 / 异步）
#   - 头文件位于 <项目>/Include/，源码位于 <项目>/Src/
#   - 生成 build/libCommon.a
# 可用目标:
#   make                        —— 构建 libCommon.a
#   make debug                  —— 调试构建（-O0）
#   make objects                —— 仅编译对象文件（不生成库）
#   make compile_commands.json  —— 使用 bear 生成编译数据库（供 clangd）
#   make compiledb              —— 使用 compiledb 生成编译数据库
#   make clean                  —— 清理构建产物与编译数据库
# ------------------------------------------------------------------

MKFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT       := $(abspath $(MKFILE_DIR)/..)
WS_ROOT    := $(abspath $(MKFILE_DIR)/../..)
PROJECT    := $(notdir $(ROOT))

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O2 -g -pthread
# 本项目 Include + 第三方子模块（asio / inih）
CPPFLAGS ?= -I$(ROOT)/Include \
            -I$(WS_ROOT)/Common/ThirdParty/asio/asio/include -DASIO_STANDALONE \
            -I$(WS_ROOT)/Common/ThirdParty/inih
LDFLAGS  ?=
LDLIBS   ?=

SRC_DIR   := $(ROOT)/Src
BUILD_DIR := $(WS_ROOT)/build
TARGET    := $(BUILD_DIR)/libCommon.a

SRCS := $(shell find $(SRC_DIR) -name '*.cpp' | sort)
OBJS := $(foreach src,$(SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(src))).o)
DEPS := $(OBJS:.o=.d)

all: $(TARGET)

# inih（第三方 C 源码，编译进 libCommon.a）
INIH_SRC := $(WS_ROOT)/Common/ThirdParty/inih/ini.c
INIH_OBJ := $(BUILD_DIR)/Common_ini.o

$(TARGET): $(OBJS) $(INIH_OBJ)
	ar rcs $@ $^

$(INIH_OBJ): $(INIH_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -c $< -o $@

.PHONY: objects
objects: $(OBJS) $(INIH_OBJ)

define compile-obj
$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(1))).o: $(1)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach src,$(SRCS),$(eval $(call compile-obj,$(src))))

-include $(DEPS)

# ------------------------------------------------------------------
# 生成 compile_commands.json（供 clangd 使用）
# ------------------------------------------------------------------

.PHONY: compile_commands.json
compile_commands.json:
	bear --output $(ROOT)/compile_commands.json -- make clean all

.PHONY: compiledb
compiledb:
	compiledb -n -o $(ROOT)/compile_commands.json make debug

.PHONY: debug
debug:
	$(MAKE) all CXXFLAGS="-std=c++11 -Wall -Wextra -O0 -g -pthread"

clean:
	rm -rf $(OBJS) $(DEPS) $(INIH_OBJ) $(TARGET) $(ROOT)/compile_commands.json

.PHONY: all debug run clean
