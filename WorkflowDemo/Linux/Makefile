# ------------------------------------------------------------------
# WorkflowDemo 项目 Makefile（Sogou Workflow 使用示例）
#   - 源码位于 <项目>/（main.cpp / demo_client.cpp / demo_parallel.cpp）
#   - 链接 workflow 静态库（ThirdParty/build.sh 独立编译产出 libworkflow.a）
#   - 生成 build/demo_server（HTTP echo 服务）
#   - 生成 build/demo_client（HTTP 客户端）
#   - 生成 build/demo_parallel（并行 HTTP 请求）
# 可用目标:
#   make                        —— 构建全部示例
#   make debug                  —— 调试构建（-O0）
#   make objects                —— 仅编译对象文件（不生成可执行）
#   make compiledb              —— 使用 compiledb 生成编译数据库（供 clangd）
#   make clean                  —— 清理构建产物与编译数据库
# ------------------------------------------------------------------

MKFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT       := $(abspath $(MKFILE_DIR)/..)
WS_ROOT    := $(abspath $(MKFILE_DIR)/../..)
PROJECT    := $(notdir $(ROOT))

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O2 -g -pthread
# 构建模式（release / debug）：产物分目录 build/<模式>/（由 build.sh 传入）
BUILD_MODE ?= release
# workflow 公共头文件（<workflow/WFHttpServer.h> 等）
CPPFLAGS ?= -I$(ROOT) -I$(WS_ROOT)/ThirdParty/workflow/src/include
LDFLAGS  ?=

SRC_DIR   := $(ROOT)
BUILD_DIR := $(WS_ROOT)/build/$(BUILD_MODE)

# 链接 workflow 静态库（nossl 分支移除了 SSL 协议，但 HTTP 内部仍用
# OpenSSL crypto 做 base64 编码，故仍需 -lcrypto）
WORKFLOW_LIB := $(BUILD_DIR)/libworkflow.a
LDLIBS   ?= $(WORKFLOW_LIB) -lcrypto -lpthread

TARGET_SERVER    := $(BUILD_DIR)/demo_server
TARGET_CLIENT    := $(BUILD_DIR)/demo_client
TARGET_PARALLEL  := $(BUILD_DIR)/demo_parallel

SERVER_SRC   := $(SRC_DIR)/main.cpp
CLIENT_SRC   := $(SRC_DIR)/demo_client.cpp
PARALLEL_SRC := $(SRC_DIR)/demo_parallel.cpp

SERVER_OBJ   := $(BUILD_DIR)/WorkflowDemo_main.o
CLIENT_OBJ   := $(BUILD_DIR)/WorkflowDemo_demo_client.o
PARALLEL_OBJ := $(BUILD_DIR)/WorkflowDemo_demo_parallel.o
DEPS := $(SERVER_OBJ:.o=.d) $(CLIENT_OBJ:.o=.d) $(PARALLEL_OBJ:.o=.d)

# 默认目标
all: $(TARGET_SERVER) $(TARGET_CLIENT) $(TARGET_PARALLEL)

$(TARGET_SERVER): $(SERVER_OBJ) $(WORKFLOW_LIB)
	$(CXX) $(LDFLAGS) $(SERVER_OBJ) $(LDLIBS) -o $@

$(TARGET_CLIENT): $(CLIENT_OBJ) $(WORKFLOW_LIB)
	$(CXX) $(LDFLAGS) $(CLIENT_OBJ) $(LDLIBS) -o $@

$(TARGET_PARALLEL): $(PARALLEL_OBJ) $(WORKFLOW_LIB)
	$(CXX) $(LDFLAGS) $(PARALLEL_OBJ) $(LDLIBS) -o $@

# 对象文件编译规则
$(SERVER_OBJ): $(SERVER_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(CLIENT_OBJ): $(CLIENT_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(PARALLEL_OBJ): $(PARALLEL_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

# workflow 静态库：缺失时用独立脚本构建
$(WORKFLOW_LIB):
	$(WS_ROOT)/ThirdParty/build.sh workflow

# 仅编译对象文件（不生成可执行），供编译数据库生成使用
.PHONY: objects
objects: $(SERVER_OBJ) $(CLIENT_OBJ) $(PARALLEL_OBJ)

# 生成编译数据库（供 clangd）
.PHONY: compiledb
compiledb:
	compiledb -n -o $(ROOT)/compile_commands.json make objects

# 调试构建（-O0 关闭优化，便于调试）
.PHONY: debug
debug:
	$(MAKE) all CXXFLAGS="-std=c++11 -Wall -Wextra -O0 -g -pthread"

# 运行 HTTP echo 服务器
.PHONY: run-server
run-server: $(TARGET_SERVER)
	$(TARGET_SERVER) $(PORT)

# 运行 HTTP 客户端
.PHONY: run-client
run-client: $(TARGET_CLIENT)
	$(TARGET_CLIENT) $(URL)

# 运行并行示例
.PHONY: run-parallel
run-parallel: $(TARGET_PARALLEL)
	$(TARGET_PARALLEL) $(URLS)

clean:
	rm -rf $(SERVER_OBJ) $(CLIENT_OBJ) $(PARALLEL_OBJ) $(DEPS) \
	       $(TARGET_SERVER) $(TARGET_CLIENT) $(TARGET_PARALLEL) \
	       $(ROOT)/compile_commands.json

.PHONY: all debug objects compiledb run-server run-client run-parallel clean
