# ------------------------------------------------------------------
# DataHub 项目 Makefile（ServerCore 骨架 + Sogou Workflow HTTP）
#   - 模块自治：每模块一个目录，头源同目录
#   - 服务器依赖 build/libServerCore.a、build/libCommon.a、build/libworkflow.a
#   - 生成 build/datahub（HTTP 数据传输服务）
# 可用目标:
#   make                        —— 构建 datahub
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
# 本项目各模块目录 + ServerCore + Common + workflow 公共头
CPPFLAGS ?= -I$(ROOT) -I$(WS_ROOT)/ServerCore -I$(WS_ROOT)/Common -I$(WS_ROOT)/ThirdParty/workflow/src/include
LDFLAGS  ?=

SRC_DIR   := $(ROOT)
BUILD_DIR := $(WS_ROOT)/build/$(BUILD_MODE)
TARGET    := $(BUILD_DIR)/datahub
LIB       := $(BUILD_DIR)/libServerCore.a
COMMON_LIB := $(BUILD_DIR)/libCommon.a
WORKFLOW_LIB := $(BUILD_DIR)/libworkflow.a

# 源码：递归收集各模块目录下的 .cpp（排除 Linux/）
SERVER_SRCS := $(shell find $(SRC_DIR) -name '*.cpp' -not -path '*/Linux/*' | sort)
SERVER_OBJS := $(foreach src,$(SERVER_SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(src))).o)
DEPS := $(SERVER_OBJS:.o=.d)

# 链接 workflow 静态库（nossl 分支移除了 SSL 协议，但 HTTP 内部仍用
# OpenSSL crypto 做 base64 编码，故仍需 -lcrypto）
LDLIBS ?= $(WORKFLOW_LIB) -lcrypto -lpthread

# 前端页面部署目录（用户目录固定位置，代码运行时从此处读取，与工作目录无关）
# index.html 引用 style.css / app.js，故整个 Web 目录一起拷贝。
WEB_SRC_DIR := $(ROOT)/Web
WEB_DEST_DIR := $(HOME)/.datahub

# 默认目标：构建可执行文件 + 部署前端页面到用户目录
all: $(TARGET) deploy-web

# 部署前端页面：将整个 Web 目录拷贝到 ~/.datahub/
.PHONY: deploy-web
deploy-web:
	@mkdir -p $(WEB_DEST_DIR)
	@cp -f $(WEB_SRC_DIR)/index.html $(WEB_DEST_DIR)/
	@cp -f $(WEB_SRC_DIR)/style.css $(WEB_DEST_DIR)/
	@cp -f $(WEB_SRC_DIR)/app.js $(WEB_DEST_DIR)/
	@echo "已部署前端页面: $(WEB_DEST_DIR)"

# 链接：本项目对象 + ServerCore + Common + workflow
$(TARGET): $(SERVER_OBJS) $(LIB) $(COMMON_LIB) $(WORKFLOW_LIB)
	$(CXX) $(LDFLAGS) $(SERVER_OBJS) $(LIB) $(COMMON_LIB) $(LDLIBS) -o $@

# 仅编译本项目对象文件（不依赖 ServerCore 库），供编译数据库生成使用
.PHONY: objects
objects: $(SERVER_OBJS)

# 为每个源文件生成对应编译规则
define compile-obj
$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(1))).o: $(1)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach src,$(SERVER_SRCS),$(eval $(call compile-obj,$(src))))

# 引入自动生成的头文件依赖
-include $(DEPS)

# 依赖的静态库：缺失时自动构建
$(LIB):
	$(MAKE) -C $(WS_ROOT)/ServerCore/Linux all

$(COMMON_LIB):
	$(MAKE) -C $(WS_ROOT)/Common/Linux all

$(WORKFLOW_LIB):
	$(MAKE) -C $(WS_ROOT)/ThirdParty/build.sh workflow

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

# 运行服务器（默认端口 8888）
.PHONY: run
run: $(TARGET)
	$(TARGET) $(PORT)

clean:
	rm -rf $(SERVER_OBJS) $(DEPS) $(TARGET) $(ROOT)/compile_commands.json

.PHONY: all debug run clean
