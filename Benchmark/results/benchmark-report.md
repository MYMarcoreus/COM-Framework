# 异步 / 协程库性能测试报告

- 生成时间：2026-09-19 11:11:51
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.37 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 39.91 μs | 39.80 μs | 41.92 μs | 41.92 μs | 1.28 μs | 25.06 K | 54484.0× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 303.3 ns | 302.9 ns | 314.3 ns | 314.3 ns | 3.9 ns | 3.30 M | 414.1× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor Post (1 thread) | 377.1 ns | 377.6 ns | 395.3 ns | 395.3 ns | 9.4 ns | 2.65 M | 514.9× | 异步执行器 fire-and-forget（与链共用同一线程池） |
| asio::post (1 thread) | 7.74 μs | 7.71 μs | 8.31 μs | 8.31 μs | 57.0 ns | 129.15 K | 10570.9× | 行业标准异步库（本项目自带） |

## 2. 异步 promise（CPromise 层开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.9 ns | 0.0 ns | 1.34 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.9 ns | 0.0 ns | 1.35 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.35 G | 1.0× | 循环内联，理论下限 |
| CPromise x1 | 621.6 ns | 621.2 ns | 642.3 ns | 659.1 ns | 9.1 ns | 1.61 M | 830.9× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x5 | 3.78 μs | 3.74 μs | 3.94 μs | 4.41 μs | 155.7 ns | 264.57 K | 5052.2× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x20 | 14.51 μs | 14.44 μs | 15.16 μs | 15.33 μs | 278.1 ns | 68.90 K | 19400.4× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x100 | 49.02 μs | 49.24 μs | 50.93 μs | 52.72 μs | 937.0 ns | 20.40 K | 65525.4× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise deep x256 (inline→post) | 112.21 μs | 112.41 μs | 116.62 μs | 120.70 μs | 2.60 μs | 8.91 K | 149985.2× | 深层链：内联 64 层后改投递，验证不爆栈 |
| CPromise fail-fast x20 | 13.16 μs | 13.18 μs | 14.93 μs | 15.30 μs | 510.8 ns | 76.01 K | 17584.7× | 失败后后续层短路（不执行层函数，仅透传结果） |

## 3. 协程（CCoroutine 顺序化）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 0.9 ns | 0.0 ns | 1.09 G | 1.0× | 直接调用，无调度 |
| CPromise single layer | 592.8 ns | 592.7 ns | 605.9 ns | 605.9 ns | 6.3 ns | 1.69 M | 645.2× | 起 promise + 单层执行 + Await |
| CCoroutine start+await | 938.8 ns | 939.9 ns | 950.5 ns | 950.5 ns | 3.3 ns | 1.07 M | 1021.8× | CoStart → 1 次 CO_AWAIT（子链）→ 完成 |
| CPromise x10 | 9.44 μs | 9.35 μs | 10.31 μs | 10.31 μs | 433.2 ns | 105.96 K | 10271.7× | 10 层 promise（构建 + 执行 + Await） |
| CCoroutine seq await x10 | 2.82 μs | 2.82 μs | 2.85 μs | 2.85 μs | 23.8 ns | 355.16 K | 3064.5× | 10 次挂起 / 恢复（每次起一条单层子链） |
| CCoroutine parallel await x10 | 3.60 μs | 3.61 μs | 3.98 μs | 3.98 μs | 51.4 ns | 277.88 K | 3916.8× | CO_AWAIT_ALL：10 条子链并行等待，一次恢复 |

## 4. 协程伸缩（长协程 / 批量并发）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine 20 awaits (1 thread) | 5.11 μs | 5.12 μs | 5.20 μs | 5.60 μs | 44.0 ns | 195.61 K | - | 单协程 20 次挂起 / 恢复（每次 await 一条单层子链） |
| batch 200 coro x3 await @1 thread | 339.89 μs | 337.54 μs | 350.97 μs | 350.97 μs | 4.93 μs | 2.94 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @2 thread | 768.90 μs | 766.25 μs | 905.60 μs | 905.60 μs | 45.32 μs | 1.30 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @4 thread | 939.26 μs | 937.37 μs | 1.01 ms | 1.01 ms | 21.85 μs | 1.06 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |

## 5. 压力测试（窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 79.75 K | 12.54 | mutex+condvar 线程池 |
| CAsyncExecutor Post (4 threads) | 78.17 K | 12.79 | 异步执行器 fire-and-forget |
| asio::post (4 threads) | 101.39 K | 9.86 | 行业标准异步库 |
| CPromise x4 (4 threads) | 15.85 K | 63.07 | 窗口 1000 条 promise（各 4 层） |
| CPromise x8 (4 threads) | 8.81 K | 113.51 | 窗口 1000 条 promise（各 8 层） |
| CCoroutine x2 await (4 threads) | 282.75 K | 3.54 | 窗口 1000 个协程（各 2 次 await） |
| mixed chain+coro+post (4 producers) | 438.46 K | 2.28 | 4 生产者 × 20000（链 / 协程 / Post 各 1/3） |
