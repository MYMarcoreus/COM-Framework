# ------------------------------------------------------------------
# DataHubAdmin 项目 Makefile（ServerCore 骨架 + Sogou Workflow HTTP 控制面）
#   - 独立的服务器 exe：承载管理网页 + 把 /api/admin/* 代理到 DataHub
#   - 服务器依赖 build/libServerCore.a、build/libCommon.a、build/libworkflow.a
#   - 生成 build/datahub-admin（服务端管理控制面）
# 可用目标:
#   make                       —— 构建 datahub-admin
#   make debug                 —— 调试构建（-O0）
#   make objects               —— 仅编译本项目对象文件
#   make compiledb             —— 使用 compiledb 生成编译数据库
#   make clean                 —— 清理构建产物与编译数据库
# ------------------------------------------------------------------

MKFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT       := $(abspath $(MKFILE_DIR)/..)
WS_ROOT    := $(abspath $(MKFILE_DIR)/../..)
PROJECT    := $(notdir $(ROOT))

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O2 -g -pthread
BUILD_MODE ?= release
CPPFLAGS ?= -I$(ROOT) -I$(WS_ROOT)/ServerCore -I$(WS_ROOT)/Common -I$(WS_ROOT)/ThirdParty/workflow/src/include
LDFLAGS  ?=

SRC_DIR   := $(ROOT)
BUILD_DIR := $(WS_ROOT)/build/$(BUILD_MODE)
TARGET    := $(BUILD_DIR)/datahub-admin
LIB       := $(BUILD_DIR)/libServerCore.a
COMMON_LIB := $(BUILD_DIR)/libCommon.a
WORKFLOW_LIB := $(BUILD_DIR)/libworkflow.a

SERVER_SRCS := $(shell find $(SRC_DIR) -name '*.cpp' -not -path '*/Linux/*' | sort)
SERVER_OBJS := $(foreach src,$(SERVER_SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(src))).o)
DEPS := $(SERVER_OBJS:.o=.d)

LDLIBS ?= $(WORKFLOW_LIB) -lcrypto -lpthread

# 管理网页部署目录（控制面静态页：index.html + admin.js）
WEB_SRC_DIR := $(ROOT)/Web
WEB_DEST_DIR := $(HOME)/.datahub-admin

all: $(TARGET) deploy-web

.PHONY: deploy-web
deploy-web:
	@mkdir -p $(WEB_DEST_DIR)
	@cp -f $(WEB_SRC_DIR)/index.html $(WEB_DEST_DIR)/
	@cp -f $(WEB_SRC_DIR)/admin.js $(WEB_DEST_DIR)/
	@echo "已部署管理页面: $(WEB_DEST_DIR)"

$(TARGET): $(SERVER_OBJS) $(LIB) $(COMMON_LIB) $(WORKFLOW_LIB)
	$(CXX) $(LDFLAGS) $(SERVER_OBJS) $(LIB) $(COMMON_LIB) $(LDLIBS) -o $@

.PHONY: objects
objects: $(SERVER_OBJS)

define compile-obj
$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(1))).o: $(1)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach src,$(SERVER_SRCS),$(eval $(call compile-obj,$(src))))

-include $(DEPS)

$(LIB):
	$(MAKE) -C $(WS_ROOT)/ServerCore/Linux all

$(COMMON_LIB):
	$(MAKE) -C $(WS_ROOT)/Common/Linux all

$(WORKFLOW_LIB):
	$(MAKE) -C $(WS_ROOT)/ThirdParty/build.sh workflow

.PHONY: compile_commands.json
compile_commands.json:
	bear --output $(ROOT)/compile_commands.json -- make clean objects

.PHONY: compiledb
compiledb:
	compiledb -n -o $(ROOT)/compile_commands.json make debug_objects

.PHONY: debug
debug:
	$(MAKE) all CXXFLAGS="-std=c++11 -Wall -Wextra -O0 -g -pthread"

.PHONY: debug_objects
debug_objects:
	$(MAKE) objects CXXFLAGS="-std=c++11 -Wall -Wextra -O0 -g -pthread"

clean:
	rm -rf $(SERVER_OBJS) $(DEPS) $(TARGET) $(ROOT)/compile_commands.json

.PHONY: all debug run clean
