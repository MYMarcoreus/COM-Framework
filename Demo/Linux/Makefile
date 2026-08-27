# ------------------------------------------------------------------
# Demo 项目 Makefile（ServerCore 验证项目）
#   - 模块自治：每模块一个目录，头源同目录
#   - 生成 build/demo（服务器）与 build/demo_client（测试客户端）
#   - 服务器依赖 build/libServerCore.a
# 可用目标:
#   make                        —— 构建 demo 与 demo_client
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
# 本项目各模块目录 + ServerCore + Common + LogServer（日志上报协议）+ asio 子模块
CPPFLAGS ?= -I$(ROOT) -I$(WS_ROOT)/ServerCore -I$(WS_ROOT)/Common -I$(WS_ROOT)/LogServer -I$(WS_ROOT)/Common/ThirdParty/asio/asio/include -DASIO_STANDALONE
LDFLAGS  ?=
LDLIBS   ?= $(BUILD_DIR)/libServerCore.a $(BUILD_DIR)/libCommon.a

SRC_DIR   := $(ROOT)
BUILD_DIR := $(WS_ROOT)/build/$(BUILD_MODE)
TARGET    := $(BUILD_DIR)/demo
CLIENT    := $(BUILD_DIR)/demo_client
LIB       := $(BUILD_DIR)/libServerCore.a
COMMON_LIB := $(BUILD_DIR)/libCommon.a

# 服务器源码（排除客户端入口与 Linux）
SERVER_SRCS := $(filter-out $(SRC_DIR)/Client/client_main.cpp,$(shell find $(SRC_DIR) -name '*.cpp' -not -path '*/Linux/*' | sort))
CLIENT_SRCS := $(SRC_DIR)/Client/client_main.cpp
# 客户端复用的协议实现源码
PROTOCOL_SRCS := $(SRC_DIR)/Protocol/DemoProtocol.cpp
# 日志上报复用的 LogServer 协议实现源码（单独命名对象，避免与 LogServer 冲突）
LOGPROTOCOL_SRC := $(WS_ROOT)/LogServer/Protocol/LogProtocol.cpp
LOGPROTOCOL_OBJ := $(BUILD_DIR)/Demo_LogProtocol.o

SERVER_OBJS := $(foreach src,$(SERVER_SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(src))).o)
CLIENT_OBJS := $(foreach src,$(CLIENT_SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(src))).o)
PROTOCOL_OBJS := $(foreach src,$(PROTOCOL_SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(src))).o)
DEPS := $(SERVER_OBJS:.o=.d) $(CLIENT_OBJS:.o=.d)

# 默认目标
all: $(TARGET) $(CLIENT)

# 服务器链接 libServerCore.a 与 libCommon.a（含日志上报协议对象）
$(TARGET): $(SERVER_OBJS) $(LOGPROTOCOL_OBJ) $(LIB) $(COMMON_LIB)
	$(CXX) $(LDFLAGS) $(SERVER_OBJS) $(LOGPROTOCOL_OBJ) $(LDLIBS) -o $@

# 客户端（复用 Demo 协议实现，不依赖 ServerCore）
$(CLIENT): $(CLIENT_OBJS) $(PROTOCOL_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@

# 仅编译本项目对象文件（不依赖 ServerCore 库），供编译数据库生成使用
.PHONY: objects
objects: $(SERVER_OBJS) $(CLIENT_OBJS) $(LOGPROTOCOL_OBJ)

# 为每个源文件生成对应编译规则
define compile-obj
$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(1))).o: $(1)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach src,$(SERVER_SRCS) $(CLIENT_SRCS),$(eval $(call compile-obj,$(src))))

# LogServer 日志上报协议对象（供日志上报模块复用）
$(LOGPROTOCOL_OBJ): $(LOGPROTOCOL_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

# 引入自动生成的头文件依赖
-include $(DEPS)
-include $(LOGPROTOCOL_OBJ:.o=.d)

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

# 方式二：compiledb（只编译本项目对象，避免递归构建 ServerCore 污染编译数据库）
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

# 运行客户端
.PHONY: run-client
run-client: $(CLIENT)
	$(CLIENT) $(PORT)

clean:
	rm -rf $(SERVER_OBJS) $(CLIENT_OBJS) $(LOGPROTOCOL_OBJ) $(DEPS) $(LOGPROTOCOL_OBJ:.o=.d) $(TARGET) $(CLIENT) $(ROOT)/compile_commands.json

.PHONY: all debug run clean
