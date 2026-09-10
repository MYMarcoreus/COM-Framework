# 异步 / 协程库性能测试报告

- 生成时间：2026-09-10 21:51:25
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.35 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 49.77 μs | 52.36 μs | 63.56 μs | 63.56 μs | 3.47 μs | 20.09 K | 67133.2× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 332.8 ns | 335.9 ns | 346.8 ns | 346.8 ns | 6.7 ns | 3.01 M | 448.9× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor Post (1 thread) | 343.0 ns | 345.4 ns | 359.9 ns | 359.9 ns | 3.6 ns | 2.92 M | 462.7× | 异步执行器 fire-and-forget（与链共用同一线程池） |
| asio::post (1 thread) | 6.65 μs | 7.17 μs | 9.70 μs | 9.70 μs | 1.73 μs | 150.40 K | 8969.2× | 行业标准异步库（本项目自带） |

## 2. 异步 promise（CPromise 层开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.8 ns | 0.8 ns | 1.0 ns | 1.1 ns | 0.0 ns | 1.21 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.8 ns | 0.8 ns | 0.9 ns | 1.1 ns | 0.0 ns | 1.26 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.8 ns | 0.8 ns | 0.9 ns | 1.0 ns | 0.0 ns | 1.23 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.8 ns | 0.8 ns | 0.9 ns | 1.0 ns | 0.0 ns | 1.29 G | 0.9× | 循环内联，理论下限 |
| CPromise x1 | 600.0 ns | 597.9 ns | 713.7 ns | 757.2 ns | 22.3 ns | 1.67 M | 723.1× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x5 | 5.45 μs | 5.42 μs | 6.02 μs | 6.67 μs | 450.6 ns | 183.61 K | 6563.7× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x20 | 17.98 μs | 17.97 μs | 20.43 μs | 25.40 μs | 1.32 μs | 55.62 K | 21665.7× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x100 | 62.96 μs | 61.64 μs | 68.45 μs | 71.99 μs | 3.61 μs | 15.88 K | 75871.2× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise deep x256 (inline→post) | 139.69 μs | 139.22 μs | 140.81 μs | 142.57 μs | 1.40 μs | 7.16 K | 168345.6× | 深层链：内联 64 层后改投递，验证不爆栈 |
| CPromise fail-fast x20 | 16.82 μs | 17.02 μs | 18.21 μs | 20.72 μs | 713.6 ns | 59.47 K | 20265.3× | 失败后后续层短路（不执行层函数，仅透传结果） |

## 3. 协程（CCoroutine 顺序化）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.07 G | 1.0× | 直接调用，无调度 |
| CPromise single layer | 647.2 ns | 654.8 ns | 687.0 ns | 687.0 ns | 30.6 ns | 1.55 M | 694.0× | 起 promise + 单层执行 + Await |
| CCoroutine start+await | 929.4 ns | 944.0 ns | 983.3 ns | 983.3 ns | 35.8 ns | 1.08 M | 996.6× | CoStart → 1 次 CO_AWAIT（子链）→ 完成 |
| CPromise x10 | 13.54 μs | 13.51 μs | 15.39 μs | 15.39 μs | 494.8 ns | 73.86 K | 14518.9× | 10 层 promise（构建 + 执行 + Await） |
| CCoroutine seq await x10 | 3.24 μs | 3.24 μs | 3.79 μs | 3.79 μs | 120.0 ns | 308.23 K | 3478.9× | 10 次挂起 / 恢复（每次起一条单层子链） |
| CCoroutine parallel await x10 | 4.20 μs | 4.34 μs | 5.65 μs | 5.65 μs | 290.9 ns | 237.87 K | 4508.0× | CO_AWAIT_ALL：10 条子链并行等待，一次恢复 |

## 4. 协程伸缩（长协程 / 批量并发）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine 20 awaits (1 thread) | 5.51 μs | 5.51 μs | 6.07 μs | 6.18 μs | 120.3 ns | 181.37 K | - | 单协程 20 次挂起 / 恢复（每次 await 一条单层子链） |
| batch 200 coro x3 await @1 thread | 369.08 μs | 368.99 μs | 450.08 μs | 450.08 μs | 22.90 μs | 2.71 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @2 thread | 801.67 μs | 791.57 μs | 1.07 ms | 1.07 ms | 67.83 μs | 1.25 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @4 thread | 1.02 ms | 1.01 ms | 1.14 ms | 1.14 ms | 70.44 μs | 978.85 | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |

## 5. 压力测试（窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 69.85 K | 14.32 | mutex+condvar 线程池 |
| CAsyncExecutor Post (4 threads) | 68.88 K | 14.52 | 异步执行器 fire-and-forget |
| asio::post (4 threads) | 114.61 K | 8.73 | 行业标准异步库 |
| CPromise x4 (4 threads) | 14.17 K | 70.59 | 窗口 1000 条 promise（各 4 层） |
| CPromise x8 (4 threads) | 8.85 K | 112.94 | 窗口 1000 条 promise（各 8 层） |
| CCoroutine x2 await (4 threads) | 235.05 K | 4.25 | 窗口 1000 个协程（各 2 次 await） |
| mixed chain+coro+post (4 producers) | 339.11 K | 2.95 | 4 生产者 × 20000（链 / 协程 / Post 各 1/3） |
