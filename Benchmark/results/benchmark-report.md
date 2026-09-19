# 异步 / 协程库性能测试报告

- 生成时间：2026-09-12 15:43:55
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.8 ns | 0.9 ns | 0.9 ns | 0.0 ns | 1.34 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 33.39 μs | 34.28 μs | 82.65 μs | 82.65 μs | 2.06 μs | 29.95 K | 44576.5× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 334.5 ns | 336.7 ns | 377.7 ns | 377.7 ns | 13.5 ns | 2.99 M | 446.5× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor Post (1 thread) | 386.2 ns | 385.1 ns | 402.1 ns | 402.1 ns | 7.4 ns | 2.59 M | 515.6× | 异步执行器 fire-and-forget（与链共用同一线程池） |
| asio::post (1 thread) | 9.79 μs | 9.69 μs | 10.84 μs | 10.84 μs | 618.1 ns | 102.16 K | 13067.6× | 行业标准异步库（本项目自带） |

## 2. 异步 promise（CPromise 层开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.8 ns | 0.8 ns | 0.8 ns | 0.9 ns | 0.0 ns | 1.29 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.8 ns | 0.8 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.30 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.8 ns | 0.8 ns | 0.8 ns | 0.9 ns | 0.0 ns | 1.32 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.8 ns | 0.8 ns | 0.8 ns | 0.9 ns | 0.0 ns | 1.31 G | 1.0× | 循环内联，理论下限 |
| CPromise x1 | 569.0 ns | 573.9 ns | 586.5 ns | 693.2 ns | 11.9 ns | 1.76 M | 733.0× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x5 | 3.77 μs | 3.74 μs | 4.01 μs | 4.72 μs | 92.7 ns | 264.92 K | 4862.9× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x20 | 15.01 μs | 15.16 μs | 15.78 μs | 16.66 μs | 357.9 ns | 66.61 K | 19340.6× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x100 | 51.97 μs | 52.16 μs | 54.85 μs | 57.25 μs | 724.5 ns | 19.24 K | 66953.5× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise deep x256 (inline→post) | 121.93 μs | 120.11 μs | 130.04 μs | 131.83 μs | 4.36 μs | 8.20 K | 157076.2× | 深层链：内联 64 层后改投递，验证不爆栈 |
| CPromise fail-fast x20 | 14.88 μs | 14.74 μs | 15.86 μs | 16.51 μs | 418.3 ns | 67.20 K | 19169.8× | 失败后后续层短路（不执行层函数，仅透传结果） |

## 3. 协程（CCoroutine 顺序化）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.7 ns | 0.7 ns | 0.0 ns | 1.35 G | 1.0× | 直接调用，无调度 |
| CPromise single layer | 603.5 ns | 603.6 ns | 690.9 ns | 690.9 ns | 26.8 ns | 1.66 M | 813.2× | 起 promise + 单层执行 + Await |
| CCoroutine start+await | 1.04 μs | 1.04 μs | 1.15 μs | 1.15 μs | 49.0 ns | 964.89 K | 1396.5× | CoStart → 1 次 CO_AWAIT（子链）→ 完成 |
| CPromise x10 | 7.84 μs | 7.84 μs | 8.18 μs | 8.18 μs | 170.3 ns | 127.48 K | 10569.7× | 10 层 promise（构建 + 执行 + Await） |
| CCoroutine seq await x10 | 3.15 μs | 3.19 μs | 3.52 μs | 3.52 μs | 65.3 ns | 317.16 K | 4248.4× | 10 次挂起 / 恢复（每次起一条单层子链） |
| CCoroutine parallel await x10 | 3.89 μs | 3.90 μs | 4.25 μs | 4.25 μs | 76.8 ns | 256.81 K | 5246.7× | CO_AWAIT_ALL：10 条子链并行等待，一次恢复 |

## 4. 协程伸缩（长协程 / 批量并发）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine 20 awaits (1 thread) | 5.24 μs | 5.26 μs | 5.72 μs | 6.37 μs | 77.7 ns | 190.81 K | - | 单协程 20 次挂起 / 恢复（每次 await 一条单层子链） |
| batch 200 coro x3 await @1 thread | 359.07 μs | 355.61 μs | 424.17 μs | 424.17 μs | 10.33 μs | 2.78 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @2 thread | 803.92 μs | 806.42 μs | 952.39 μs | 952.39 μs | 33.05 μs | 1.24 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @4 thread | 956.11 μs | 953.77 μs | 1.01 ms | 1.01 ms | 32.36 μs | 1.05 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |

## 5. 压力测试（窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 79.05 K | 12.65 | mutex+condvar 线程池 |
| CAsyncExecutor Post (4 threads) | 77.34 K | 12.93 | 异步执行器 fire-and-forget |
| asio::post (4 threads) | 99.21 K | 10.08 | 行业标准异步库 |
| CPromise x4 (4 threads) | 16.05 K | 62.31 | 窗口 1000 条 promise（各 4 层） |
| CPromise x8 (4 threads) | 8.91 K | 112.23 | 窗口 1000 条 promise（各 8 层） |
| CCoroutine x2 await (4 threads) | 273.82 K | 3.65 | 窗口 1000 个协程（各 2 次 await） |
| mixed chain+coro+post (4 producers) | 448.29 K | 2.23 | 4 生产者 × 20000（链 / 协程 / Post 各 1/3） |
