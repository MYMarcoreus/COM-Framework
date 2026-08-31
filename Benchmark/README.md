# Benchmark — 异步 / 协程库性能与压力测试

对 `Common` 的异步、协程库做**带基准比较**的性能测试，结果以 markdown 表格汇总。

## 被测对象

| 实现 | 位置 | 说明 |
|---|---|---|
| `common::thread::CThreadPool` | `Common/Thread/` | mutex + condition_variable 线程池 |
| `common::async::CAsyncExecutor` | `Common/Async/` | 无异常任务链框架（Option 风格） |
| `common::async::CCoroutine` | `Common/Async/Coroutine.h` | 基于任务链的无栈协程 |
| `asio::post` | `ThirdParty/asio` | 行业标准第三方异步库（对比基线） |
| `direct_call` | — | 直接函数调用（理论下限） |
| `std::thread` | 标准库 | 每任务新建线程（最重基线） |

## 测试维度

1. **任务提交**：单任务「提交 → 执行 → 通知 → 唤醒」端到端往返延迟（ns/op、P50、P99、吞吐）。
2. **任务链**：`CAsyncExecutor::Then` 链（1/5/20 级）构建 + 执行 + 取值成本。
3. **协程**：`CCoroutine` 启动 + 挂起/恢复 + 完成，以及 10 级 await 链。
4. **协程创建/切换**（对齐 [librf](https://github.com/tearshark/librf) 的 `resumable_switch` 方法）：
   CoStart 创建成本与每次 `CO_AWAIT` 挂起/恢复的切换成本**分离计时**，
   按协程数量梯度（100/1000/10000）扫描扩展性。
5. **压力测试**：4 线程窗口式稳定吞吐（ops/s）+ 大窗口背压 + Stop 排空延迟。

## 构建与运行

```bash
# 推荐：release（-O2）构建并运行
./build.sh -r Benchmark && ./build/release/benchmark

# 只构建
./build.sh -r Benchmark

# debug 模式（-O0，仅观察相对趋势，绝对数值请以 release 为准）
./build.sh -d Benchmark && ./build/debug/benchmark
```

`Benchmark/Linux/Makefile` 自动纳入 `build.sh` 的项目自动发现，无需手动注册。

## 结果

- 终端直接打印 markdown 表格。
- 同时写入 `Benchmark/results/benchmark-report.md`。

## 目录结构

```text
Benchmark/
├── Linux/Makefile        # 构建（输出 build/<mode>/benchmark）
├── main.cpp              # 入口：运行所有用例 + 汇总
├── framework/
│   ├── Bench.h           # 轻量微基准 + 窗口式压力框架（仅 C++11 标准库）
│   └── Report.h          # markdown 表格汇总输出
├── cases/
│   ├── Engines.h         # CThreadPool / CAsyncExecutor / asio 引擎封装
│   ├── SubmitCase.*      # 任务提交往返延迟
│   ├── ChainCase.*       # 任务链开销（含 100 级深层链）
│   ├── CoroutineCase.*   # 协程启动 / 挂起 / 链
│   ├── ResumableCase.*   # 协程创建 / 切换成本（librf 风格，数量梯度）
│   └── StressCase.*      # 窗口式吞吐（含多线程协程/链）+ 停止延迟
└── results/
    └── benchmark-report.md   # 生成的测试报告（运行后出现）
```

## 扩展新用例

1. 在 `cases/` 新增 `FooCase.h/.cpp`（导出 `void RunFooCases()`）。
2. 在 `main.cpp` 中调用 `RunFooCases()`。
3. 用 `benchmark::BenchOp(...)` 做微基准、`benchmark::StressWindow(...)` 做压力测试，
   结果自动进入汇总表格。

## 注意事项

- 性能测试对编译优化敏感：**务必用 `-r`（-O2）跑**，`-d` 仅供调试。
- 为公平对比，三种异步引擎统一使用「fire-and-forget 提交 + 原子计数等待完成」，
  `CAsyncExecutor` 的 `Post` 与 `Submit().Get()` 是不同路径，可自行扩展用例对比。
- 协程用例中的链协程是固定展开的 10 级 `CO_AWAIT`（Duff's device 状态机限制
  宏不能在同一行重复使用）。
