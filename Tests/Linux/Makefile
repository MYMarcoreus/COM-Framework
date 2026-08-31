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
# 构建模式（release / debug）：产物分目录 build/<模式>/（由 build.sh 传入）
BUILD_MODE ?= release
# 本项目 + 被测库 + LogServer（被测业务项目）+ 第三方子模块（asio / inih）
CPPFLAGS ?= -I$(ROOT) \
            -I$(WS_ROOT)/Common \
            -I$(WS_ROOT)/ServerCore \
            -I$(WS_ROOT)/LogServer \
            -I$(WS_ROOT)/ThirdParty/asio/asio/include -DASIO_STANDALONE \
            -I$(WS_ROOT)/ThirdParty/inih

SRC_DIR   := $(ROOT)
BUILD_DIR := $(WS_ROOT)/build/$(BUILD_MODE)
TARGET    := $(BUILD_DIR)/tests
LIBS      := $(BUILD_DIR)/libCommon.a $(BUILD_DIR)/libServerCore.a

# 被测业务项目源码（LogServer 协议与存储），单独命名对象避免冲突
LOGSERVER_SRCS := $(WS_ROOT)/LogServer/Protocol/LogProtocol.cpp \
                  $(WS_ROOT)/LogServer/Service/LogStorage.cpp
LOGSERVER_OBJS := $(BUILD_DIR)/Tests_LogProtocol.o $(BUILD_DIR)/Tests_LogStorage.o

SRCS := $(shell find $(SRC_DIR) -name '*.cpp' -not -path '*/Linux/*' | sort)
OBJS := $(foreach src,$(SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(src))).o)
DEPS := $(OBJS:.o=.d)

all: $(TARGET)

.PHONY: run
run: $(TARGET)
	$(TARGET)

$(TARGET): $(OBJS) $(LOGSERVER_OBJS) $(LIBS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LOGSERVER_OBJS) $(LIBS)

define compile-obj
$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(1))).o: $(1)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach src,$(SRCS),$(eval $(call compile-obj,$(src))))

# LogServer 被测源码对象
$(BUILD_DIR)/Tests_LogProtocol.o: $(WS_ROOT)/LogServer/Protocol/LogProtocol.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/Tests_LogStorage.o: $(WS_ROOT)/LogServer/Service/LogStorage.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

.PHONY: compiledb
compiledb:
	compiledb -n -o $(ROOT)/compile_commands.json make all

.PHONY: clean
clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) $(LOGSERVER_OBJS) $(LOGSERVER_OBJS:.o=.d) compile_commands.json
