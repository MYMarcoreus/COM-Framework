# Benchmark — 异步 / 协程库性能与压力测试

对 `Common` 的异步、协程库做**带基准比较**的性能测试，结果以 markdown 表格汇总。

## 被测对象

| 实现 | 位置 | 说明 |
|---|---|---|
| `common::thread::CThreadPool` | `Common/Thread/` | mutex + condition_variable 线程池 |
| `common::async::CAsyncExecutor` | `Common/Async/` | 异步链框架的调度层（线程池 + `Post`） |
| `common::async::CAsyncChain` | `Common/Async/AsyncChain.h` | 异步链（固定签名层 + 共享上下文，失败即停） |
| `common::async::CCoroutine` | `Common/Async/Coroutine.h` | 基于异步链的无栈协程（await 链） |
| `asio::post` | `ThirdParty/asio` | 行业标准第三方异步库（对比基线） |
| `direct_call` | — | 直接函数调用（理论下限） |
| `std::thread` | 标准库 | 每任务新建线程（最重基线） |

## 测试维度

1. **任务提交**：单任务「提交 → 执行 → 通知 → 唤醒」端到端往返延迟（ns/op、P50、P99、吞吐）。
2. **异步链**：`CAsyncChain` 链（1/5/20/100 层）构建 + 逐层级联 + 取值成本，
   含深链（256 层，超过内联深度上限后改投递）与失败即停（短路）链。
3. **协程**：`CCoroutine` 启动 + 一次 await + 完成，以及 10 次顺序 / 并行 await
   与等效链的对比。
4. **协程伸缩**：长协程（单协程 20 次挂起/恢复）与批量协程（200 个 × 3 await）
   在 1 / 2 / 4 线程执行器下的总成本。
5. **压力测试**：窗口式稳定吞吐（ops/s）——引擎对比 + 链压力 + 协程压力 + 混合负载。

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
│   ├── Engines.h         # CThreadPool / CAsyncExecutor(Post) / asio 引擎封装
│   ├── ChainContext.h    # 基准用共享上下文与层函数
│   ├── SubmitCase.*      # 任务提交往返延迟
│   ├── ChainCase.*       # 异步链开销（1/5/20/100 层 + 深链 256 + 失败即停）
│   ├── CoroutineCase.*   # 协程启动 / 顺序 await / 并行 await
│   ├── ResumableCase.*   # 长协程挂起恢复 + 批量协程多线程伸缩
│   └── StressCase.*      # 窗口式吞吐（引擎 / 链 / 协程 / 混合负载）
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
- 为公平对比，三种 fire-and-forget 引擎统一使用「提交 + 原子计数等待完成」；
  链 / 协程用例直接使用 `CAsyncExecutor::Submit` / `CoStart`（它们不支持 fire-and-forget）。
- 协程体内的 await 次数是**源码固定写出**的（Duff's device 状态机用 `__LINE__`
  作恢复点，宏不能循环展开）；基准里的链程已固定展开 10 / 20 次。
