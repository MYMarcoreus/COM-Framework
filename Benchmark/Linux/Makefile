# ------------------------------------------------------------------
# Benchmark 性能 / 压力测试 Makefile
#   - 链接 build/libCommon.a，生成 build/<mode>/benchmark
#   - 结果以 markdown 表格写入 Benchmark/results/benchmark-report.md
#   - 依赖 Common（异步 / 线程 / 协程被测库）
# 可用目标:
#   make            —— 构建 build/<mode>/benchmark
#   make run        —— 构建并运行
#   make compiledb  —— 生成 compile_commands.json（clangd）
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
# 本项目 + 被测库 Common + 第三方子模块（asio）
CPPFLAGS ?= -I$(ROOT) \
            -I$(WS_ROOT)/Common \
            -I$(WS_ROOT)/ThirdParty/asio/asio/include -DASIO_STANDALONE \
            -DBENCHMARK_RESULT_DIR='"$(WS_ROOT)/Benchmark/results"'

SRC_DIR   := $(ROOT)
BUILD_DIR := $(WS_ROOT)/build/$(BUILD_MODE)
TARGET    := $(BUILD_DIR)/benchmark
LIBS      := $(BUILD_DIR)/libCommon.a

SRCS := $(shell find $(SRC_DIR) -name '*.cpp' -not -path '*/Linux/*' | sort)
OBJS := $(foreach src,$(SRCS),$(BUILD_DIR)/$(PROJECT)_$(subst /,_,$(patsubst $(SRC_DIR)/%.cpp,%,$(src))).o)
DEPS := $(OBJS:.o=.d)

all: $(TARGET)

.PHONY: run
run: $(TARGET)
	$(TARGET)

$(TARGET): $(OBJS) $(LIBS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LIBS)

# 依赖 Common 静态库（若未构建则递归先构建）
$(BUILD_DIR)/libCommon.a:
	$(MAKE) -C $(WS_ROOT)/Common/Linux all BUILD_MODE=$(BUILD_MODE)

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
	rm -f $(OBJS) $(DEPS) $(TARGET) $(ROOT)/compile_commands.json
