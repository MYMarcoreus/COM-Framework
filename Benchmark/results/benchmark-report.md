# 异步 / 协程库性能测试报告

- 生成时间：2026-09-19 21:56:14
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 ns | 0.8 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.31 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 54.97 μs | 54.55 μs | 71.80 μs | 71.80 μs | 3.52 μs | 18.19 K | 72284.5× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 377.3 ns | 400.4 ns | 613.8 ns | 613.8 ns | 35.1 ns | 2.65 M | 496.1× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor Post (1 thread) | 615.0 ns | 626.0 ns | 749.1 ns | 749.1 ns | 22.0 ns | 1.63 M | 808.7× | 异步执行器 fire-and-forget（与链共用同一线程池） |
| asio::post (1 thread) | 4.01 μs | 4.11 μs | 5.22 μs | 5.22 μs | 298.3 ns | 249.68 K | 5266.4× | 行业标准异步库（本项目自带） |

## 2. 异步 promise（CPromise 层开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.8 ns | 0.8 ns | 0.9 ns | 1.2 ns | 0.0 ns | 1.27 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.8 ns | 0.8 ns | 0.8 ns | 1.1 ns | 0.0 ns | 1.30 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.8 ns | 0.8 ns | 0.9 ns | 1.4 ns | 0.0 ns | 1.25 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.8 ns | 0.8 ns | 0.9 ns | 1.1 ns | 0.0 ns | 1.21 G | 1.0× | 循环内联，理论下限 |
| CPromise x1 | 841.7 ns | 845.9 ns | 1.14 μs | 1.46 μs | 50.6 ns | 1.19 M | 1069.3× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x5 | 4.46 μs | 4.58 μs | 5.47 μs | 8.31 μs | 427.0 ns | 224.03 K | 5670.4× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x20 | 16.90 μs | 17.14 μs | 20.32 μs | 21.94 μs | 1.29 μs | 59.17 K | 21470.8× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x100 | 55.66 μs | 55.17 μs | 59.44 μs | 59.63 μs | 1.73 μs | 17.97 K | 70709.4× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise deep x256 (inline→post) | 156.48 μs | 149.20 μs | 181.78 μs | 187.18 μs | 17.84 μs | 6.39 K | 198780.3× | 深层链：内联 64 层后改投递，验证不爆栈 |
| CPromise fail-fast x20 | 14.80 μs | 14.83 μs | 18.10 μs | 23.78 μs | 740.9 ns | 67.55 K | 18805.9× | 失败后后续层短路（不执行层函数，仅透传结果） |

## 3. 协程（CCoroutine 顺序化）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 ns | 0.8 ns | 1.0 ns | 1.0 ns | 0.1 ns | 1.22 G | 1.0× | 直接调用，无调度 |
| CPromise single layer | 872.5 ns | 877.0 ns | 1.38 μs | 1.38 μs | 127.8 ns | 1.15 M | 1064.6× | 起 promise + 单层执行 + Await |
| CCoroutine start+await | 1.36 μs | 1.38 μs | 1.79 μs | 1.79 μs | 88.7 ns | 733.87 K | 1662.7× | CoStart → 1 次 CO_AWAIT（子链）→ 完成 |
| CPromise x10 | 10.31 μs | 10.51 μs | 12.90 μs | 12.90 μs | 1.07 μs | 97.03 K | 12575.3× | 10 层 promise（构建 + 执行 + Await） |
| CCoroutine seq await x10 | 4.45 μs | 4.47 μs | 4.98 μs | 4.98 μs | 74.8 ns | 224.76 K | 5428.9× | 10 次挂起 / 恢复（每次起一条单层子链） |
| CCoroutine parallel await x10 | 5.38 μs | 5.43 μs | 5.73 μs | 5.73 μs | 218.7 ns | 185.85 K | 6565.6× | CO_AWAIT_ALL：10 条子链并行等待，一次恢复 |

## 4. 协程伸缩（长协程 / 批量并发）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine 20 awaits (1 thread) | 7.50 μs | 7.51 μs | 8.43 μs | 8.67 μs | 54.9 ns | 133.27 K | - | 单协程 20 次挂起 / 恢复（每次 await 一条单层子链） |
| batch 200 coro x3 await @1 thread | 524.13 μs | 516.82 μs | 866.38 μs | 866.38 μs | 16.06 μs | 1.91 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @2 thread | 2.18 ms | 2.28 ms | 2.74 ms | 2.74 ms | 246.57 μs | 459.05 | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @4 thread | 4.13 ms | 4.17 ms | 4.51 ms | 4.51 ms | 112.14 μs | 242.09 | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |

## 5. 压力测试（窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 72.51 K | 13.79 | mutex+condvar 线程池 |
| CAsyncExecutor Post (4 threads) | 619.21 K | 1.61 | 异步执行器 fire-and-forget |
| asio::post (4 threads) | 116.65 K | 8.57 | 行业标准异步库 |
| CPromise x4 (4 threads) | 79.16 K | 12.63 | 窗口 1000 条 promise（各 4 层） |
| CPromise x8 (4 threads) | 33.75 K | 29.63 | 窗口 1000 条 promise（各 8 层） |
| CCoroutine x2 await (4 threads) | 52.93 K | 18.89 | 窗口 1000 个协程（各 2 次 await） |
| mixed chain+coro+post (4 producers) | 118.00 K | 8.47 | 4 生产者 × 20000（链 / 协程 / Post 各 1/3） |

## 6. 读写门（同一批任务：读并发 vs 写独占）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call × 256 (baseline) | 10.49 ms | 10.44 ms | 11.87 ms | 11.87 ms | 644.67 μs | 95.35 | 1.0× | 直接顺序执行同一批工作（无调度） |
| Post(kRead) × 256（读：并发） | 3.15 ms | 3.10 ms | 3.59 ms | 3.59 ms | 165.73 μs | 317.07 | 0.3× | 读任务可同时进入（上限 = 执行器线程数） |
| Post(kWrite) × 256（写：独占） | 12.68 ms | 12.75 ms | 13.52 ms | 13.52 ms | 452.67 μs | 78.89 | 1.2× | 写任务互斥（同一执行器内串行） |
