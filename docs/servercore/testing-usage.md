# 测试方法 — 使用文档

> 测试框架实现见：[testing-impl.md](testing-impl.md)

## 1. 单元测试（Tests）

轻量测试框架（`Tests/TestFramework.h`）：`TEST(name)` + `ASSERT_TRUE` / `ASSERT_EQ`。

```cpp
TEST(MyModule_DoSomething)
{
    MyModule module;
    ASSERT_TRUE(module.Do());
    ASSERT_EQ(module.Count(), 3);
}
```

### 运行

```bash
./build/release/tests     # 运行全部用例
make -C Tests/Linux run   # 或通过 Makefile
```

### 用例文件划分

| 文件 | 覆盖 |
|---|---|
| `test_common.cpp` | Common 基础库（缓冲、线程池、定时器、配置、异步等） |
| `test_exec.cpp` | Exec 并发调度框架（读写调度、公平 FIFO、压力、负载模拟） |
| `test_servercore.cpp` | 模块生命周期 / 管理器编排 / 事件 / 消息路由 / 自持引用 |
| `test_infra.cpp` | 拓扑排序 / 多实例注册 / 配置热加载 / 指标 / 连接上下文 / 异步事件 |
| `test_logserver.cpp` | 日志协议 / 存储 |
| `test_serialization.cpp` | 二进制序列化 |

## 2. 测试模块的依赖注入

```cpp
sc::CModuleManager manager;
manager.RegisterModule(sc::IID_IConfig(), new sc::CConfigModule());
manager.RegisterModule(sc::IID_IMetrics(), new sc::CMetricsModule());
manager.RegisterModule(sc::IID_IEventDispatcher(), new sc::CEventDispatcher());

sc::CResolveContext ctx(manager);   // 被测模块
MyModule module;
ASSERT_TRUE(module.Initialize(ctx));
```

## 3. 生命周期驱动

```cpp
ASSERT_TRUE(manager.InitializeAll());
ASSERT_TRUE(manager.StartAll());
// ... 验证
manager.StopAll();
manager.ShutdownAll();
```

## 4. 端到端验证（e2e）

以 ServerExample ↔ LogServer 为例：

```bash
# 终端 1：LogServer（日志收集）
cd LogServer && ../build/logserver
# 终端 2：ServerExample（日志生产者 + 回显服务器）
cd ServerExample && ../build/example
# 终端 3：客户端验证回显
./build/example_client 9000     # 期望：PING→PONG，ECHO→"Hello ServerCore"
# 状态报告（含指标聚合）
kill -USR1 $(pgrep -f build/example)
tail -25 ServerExample/example.log
```

### 验证要点

半包/粘包、异步回显、连接上下文（关闭日志出现「共接收 N 字节」）、指标计数、优雅退出（SIGINT/SIGTERM 逆序停止）。

## 5. 并发测试要点（Exec 框架）

- 断言**只允许在主测试线程执行**；工作线程仅更新原子状态，测试结束后由主线程断言
  （子线程抛异常会 `std::terminate`）；
- 用 `WaitUntil(cond, timeoutMs)` 轮询等待完成，勿依赖固定 sleep；
- 读写互斥不变量**按模块**独立统计（跨模块允许并发，全局计数会误报）。

## 6. 编译数据库

```bash
./build.sh --compiledb     # 修改代码后重新生成 compile_commands.json 供 clangd
```

## 7. 陷阱

- clangd 可能显示过期诊断：以 `clangd --check` 与 g++ 构建为准；
- `make clean` 会删除项目根 `compile_commands.json`，重建后需重跑生成脚本。
