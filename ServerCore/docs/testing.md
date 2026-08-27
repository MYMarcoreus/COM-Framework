# 测试方法

## 单元测试（Tests）

轻量测试框架（`Tests/TestFramework.h`）：`TEST(name)` + `ASSERT_TRUE` / `ASSERT_EQ`。

### 运行

```bash
./build/tests          # 运行全部用例
make -C Tests/Linux run   # 或通过 Makefile
```

### 现有用例（33 个）

| 文件 | 覆盖 |
|---|---|
| `test_common.cpp` | Common 基础库（日志、配置、网络、缓冲等） |
| `test_servercore.cpp` | 模块生命周期 / 管理器编排 / 事件 / 消息路由 / 自持引用 |
| `test_infra.cpp` | 拓扑排序 / 多实例注册 / 配置热加载 / 指标 / 连接上下文 / 异步事件 |
| `test_logserver.cpp` | 日志协议 / 存储 |
| `test_serialization.cpp` | 二进制序列化 |

### 测试模块的依赖注入

模块 `Initialize(ctx)` 用注入上下文解析依赖，单测可构造 `CModuleManager` +
真实或 mock 模块，再构造 `CResolveContext` 注入：

```cpp
sc::CModuleManager manager;
manager.RegisterModule(sc::IID_IConfig(), new sc::CConfigModule());
manager.RegisterModule(sc::IID_IMetrics(), new sc::CMetricsModule());
manager.RegisterModule(sc::IID_IEventDispatcher(), new sc::CEventDispatcher());

// 被测模块
sc::CResolveContext ctx(manager);
MyModule module;
ASSERT_TRUE(module.Initialize(ctx));
```

### 生命周期驱动

通过管理器统一驱动（会走完整状态机 + 拓扑排序）：

```cpp
ASSERT_TRUE(manager.InitializeAll());
ASSERT_TRUE(manager.StartAll());
// ... 验证
manager.StopAll();
manager.ShutdownAll();
```

## 端到端验证（e2e）

以 Demo ↔ LogServer 为例：

```bash
# 终端 1：LogServer（日志收集）
cd LogServer && ../build/logserver

# 终端 2：Demo（日志生产者 + 回显服务器）
cd Demo && ../build/demo

# 终端 3：客户端验证回显
./build/demo_client 9000
# 期望：PING→PONG，ECHO→"Hello ServerCore"

# 验证日志上报落盘
cat LogServer/logs/demo.log
# 期望出现 "Demo 服务器运行中，日志上报模块正常"

# 状态报告（含指标聚合）
kill -USR1 $(pgrep -f build/demo)
tail -25 Demo/demo.log
# 期望：[Status] 模块列表 + [metrics] 指标段
```

### 验证要点

- 半包 / 粘包：通过 `demo_client` 连续请求 + 日志确认顺序；
- 异步回显：服务器日志出现 `异步处理 PING / ECHO`；
- 连接上下文：关闭日志出现 `共接收 N 字节`；
- 指标：`network.accepted` / `network.conns` / `demo.echo` 等计数正确；
- 优雅退出：SIGINT / SIGTERM 后各模块逆序停止，进程干净退出。

## 编译数据库

修改代码后重新生成 `compile_commands.json` 供 clangd：

```bash
./build.sh --compiledb
```

## 陷阱

- clangd 可能显示过期诊断：以 `clangd --check` 与 g++ 构建为准；
- `make clean` 会删除项目根 `compile_commands.json`，重建后需重跑生成脚本；
- 异步测试需轮询等待（如 `for i in 0..N && !flag { sleep }`），勿依赖固定 sleep。
