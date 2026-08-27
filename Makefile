# ====================================================================
# COM-Framework 统一构建入口
#
# 用法（在项目根目录执行）：
#   make / make all     —— 全量构建：所有项目 + examples（一键）
#   make examples       —— 仅构建示例（nothrow_demo）
#   make run            —— 构建并运行示例
#   make tests          —— 构建并运行单元测试（./build/tests）
#   make debug          —— 调试构建：所有项目 + examples 用 -O0（供 VS Code 调试）
#   make compiledb      —— 生成所有项目 + examples 的 compile_commands.json（clangd）
#   make clean          —— 清理所有构建产物
#   make help           —— 显示本帮助
# ====================================================================

.PHONY: all examples run tests debug compiledb clean help

all:
	./build.sh

examples:
	./build.sh --examples

run:
	./build.sh --examples && ./build/examples/nothrow_demo

tests:
	./build.sh --tests

debug:
	./build.sh --debug

compiledb:
	./build.sh --compiledb

clean:
	./build.sh --clean

help:
	./build.sh --help
