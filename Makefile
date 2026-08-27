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
	@echo "==================== 全量构建（项目 + examples） ===================="
	bash build-all.sh
	$(MAKE) -C examples
	@echo "==================== 全量构建完成 ===================="

examples:
	$(MAKE) -C examples

run:
	$(MAKE) -C examples run

tests:
	bash build-all.sh
	./build/tests

# 调试构建：全项目 -O0（含 examples）。注意会覆盖 build/ 下的发布产物，
# 想恢复发布版请执行 make / bash build-all.sh。
debug:
	bash build-debug.sh

compiledb:
	bash generate-compiledb.sh

clean:
	bash -c 'for p in Common ServerCore LogServer Demo ServerA Tests; do make -C "$$PWD/$$p/Linux" clean; done'
	$(MAKE) -C examples clean
	@echo "==================== 已清理所有构建产物 ===================="

help:
	@echo "COM-Framework 统一构建入口："
	@echo "  make / make all    全量构建（所有项目 + examples）"
	@echo "  make examples      仅构建示例（nothrow_demo）"
	@echo "  make run           构建并运行示例"
	@echo "  make tests         构建并运行单元测试"
	@echo "  make debug         调试构建（-O0，含 examples，供 VS Code 调试）"
	@echo "  make compiledb     生成所有 compile_commands.json（clangd）"
	@echo "  make clean         清理所有构建产物"
