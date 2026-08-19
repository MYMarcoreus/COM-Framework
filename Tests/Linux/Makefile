# ------------------------------------------------------------------
# Tests 单元测试 Makefile（轻量测试框架，覆盖 Common / ServerCore）
#   - 头文件位于 <项目>/Include/，源码位于 <项目>/Src/
#   - 链接 build/libCommon.a 与 build/libServerCore.a，生成 build/tests
# 可用目标:
#   make            —— 构建 build/tests
#   make run        —— 构建并运行全部单元测试
#   make clean      —— 清理构建产物与编译数据库
# ------------------------------------------------------------------

MKFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT       := $(abspath $(MKFILE_DIR)/..)
WS_ROOT    := $(abspath $(MKFILE_DIR)/../..)
PROJECT    := $(notdir $(ROOT))

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O2 -g -pthread
# 本项目 Include + 被测库 Include + 第三方子模块（asio / inih）
CPPFLAGS ?= -I$(ROOT)/Include \
            -I$(WS_ROOT)/Common/Include \
            -I$(WS_ROOT)/ServerCore/Include \
            -I$(WS_ROOT)/Common/ThirdParty/asio/asio/include -DASIO_STANDALONE \
            -I$(WS_ROOT)/Common/ThirdParty/inih

SRC_DIR   := $(ROOT)/Src
BUILD_DIR := $(WS_ROOT)/build
TARGET    := $(BUILD_DIR)/tests
LIBS      := $(BUILD_DIR)/libCommon.a $(BUILD_DIR)/libServerCore.a

SRCS := $(shell find $(SRC_DIR) -name '*.cpp' | sort)
OBJS := $(foreach src,$(SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(src))).o)
DEPS := $(OBJS:.o=.d)

all: $(TARGET)

.PHONY: run
run: $(TARGET)
	$(TARGET)

$(TARGET): $(OBJS) $(LIBS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LIBS)

define compile-obj
$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(1))).o: $(1)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach src,$(SRCS),$(eval $(call compile-obj,$(src))))

-include $(DEPS)

.PHONY: compiledb
compiledb:
	compiledb -n -o $(ROOT)/compile_commands.json make all

.PHONY: clean
clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) compile_commands.json
