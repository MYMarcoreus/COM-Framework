# 异步 / 协程库性能测试报告

- 生成时间：2026-09-19 14:51:05
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 0.9 ns | 0.0 ns | 1.11 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 40.01 μs | 39.95 μs | 48.02 μs | 48.02 μs | 1.18 μs | 25.00 K | 44570.0× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 330.1 ns | 333.4 ns | 452.6 ns | 452.6 ns | 11.5 ns | 3.03 M | 367.8× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor Post (1 thread) | 333.9 ns | 336.0 ns | 365.6 ns | 365.6 ns | 7.5 ns | 2.99 M | 372.0× | 异步执行器 fire-and-forget（与链共用同一线程池） |
| asio::post (1 thread) | 10.13 μs | 10.30 μs | 13.78 μs | 13.78 μs | 728.6 ns | 98.69 K | 11288.8× | 行业标准异步库（本项目自带） |

## 2. 异步 promise（CPromise 层开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 1.0 ns | 0.0 ns | 1.11 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 1.0 ns | 0.0 ns | 1.11 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 0.9 ns | 0.0 ns | 1.11 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 1.0 ns | 0.0 ns | 1.12 G | 1.0× | 循环内联，理论下限 |
| CPromise x1 | 579.1 ns | 581.1 ns | 603.8 ns | 678.2 ns | 11.9 ns | 1.73 M | 642.9× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x5 | 3.50 μs | 3.48 μs | 3.77 μs | 4.19 μs | 94.9 ns | 286.07 K | 3880.8× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x20 | 15.52 μs | 15.45 μs | 16.03 μs | 16.53 μs | 201.6 ns | 64.43 K | 17229.9× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x100 | 49.31 μs | 49.62 μs | 51.81 μs | 54.80 μs | 849.9 ns | 20.28 K | 54748.5× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise deep x256 (inline→post) | 113.32 μs | 113.11 μs | 118.34 μs | 126.73 μs | 1.84 μs | 8.82 K | 125810.8× | 深层链：内联 64 层后改投递，验证不爆栈 |
| CPromise fail-fast x20 | 14.18 μs | 14.21 μs | 15.00 μs | 16.26 μs | 286.5 ns | 70.53 K | 15739.8× | 失败后后续层短路（不执行层函数，仅透传结果） |

## 3. 协程（CCoroutine 顺序化）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.7 ns | 0.7 ns | 0.0 ns | 1.39 G | 1.0× | 直接调用，无调度 |
| CPromise single layer | 590.1 ns | 589.8 ns | 640.9 ns | 640.9 ns | 12.9 ns | 1.69 M | 820.1× | 起 promise + 单层执行 + Await |
| CCoroutine start+await | 972.6 ns | 969.6 ns | 1.01 μs | 1.01 μs | 22.1 ns | 1.03 M | 1351.6× | CoStart → 1 次 CO_AWAIT（子链）→ 完成 |
| CPromise x10 | 9.60 μs | 9.49 μs | 10.09 μs | 10.09 μs | 418.7 ns | 104.13 K | 13345.5× | 10 层 promise（构建 + 执行 + Await） |
| CCoroutine seq await x10 | 2.91 μs | 2.87 μs | 3.10 μs | 3.10 μs | 79.6 ns | 344.15 K | 4038.1× | 10 次挂起 / 恢复（每次起一条单层子链） |
| CCoroutine parallel await x10 | 3.55 μs | 3.54 μs | 3.77 μs | 3.77 μs | 63.1 ns | 281.92 K | 4929.5× | CO_AWAIT_ALL：10 条子链并行等待，一次恢复 |

## 4. 协程伸缩（长协程 / 批量并发）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine 20 awaits (1 thread) | 4.72 μs | 4.72 μs | 4.79 μs | 5.03 μs | 37.0 ns | 211.66 K | - | 单协程 20 次挂起 / 恢复（每次 await 一条单层子链） |
| batch 200 coro x3 await @1 thread | 336.49 μs | 341.48 μs | 379.36 μs | 379.36 μs | 6.28 μs | 2.97 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @2 thread | 811.70 μs | 797.25 μs | 912.60 μs | 912.60 μs | 50.41 μs | 1.23 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @4 thread | 909.41 μs | 919.34 μs | 945.87 μs | 945.87 μs | 18.58 μs | 1.10 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |

## 5. 压力测试（窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 88.94 K | 11.24 | mutex+condvar 线程池 |
| CAsyncExecutor Post (4 threads) | 82.10 K | 12.18 | 异步执行器 fire-and-forget |
| asio::post (4 threads) | 101.26 K | 9.88 | 行业标准异步库 |
| CPromise x4 (4 threads) | 16.48 K | 60.69 | 窗口 1000 条 promise（各 4 层） |
| CPromise x8 (4 threads) | 9.70 K | 103.13 | 窗口 1000 条 promise（各 8 层） |
| CCoroutine x2 await (4 threads) | 286.41 K | 3.49 | 窗口 1000 个协程（各 2 次 await） |
| mixed chain+coro+post (4 producers) | 458.58 K | 2.18 | 4 生产者 × 20000（链 / 协程 / Post 各 1/3） |
