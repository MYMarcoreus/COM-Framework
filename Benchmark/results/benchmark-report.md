# 异步 / 协程库性能测试报告

- 生成时间：2026-09-19 19:08:07
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 ns | 0.8 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.32 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 42.60 μs | 42.62 μs | 48.52 μs | 48.52 μs | 4.89 μs | 23.47 K | 56096.7× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 359.6 ns | 362.3 ns | 440.3 ns | 440.3 ns | 51.8 ns | 2.78 M | 473.5× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor Post (1 thread) | 519.4 ns | 524.3 ns | 749.0 ns | 749.0 ns | 14.6 ns | 1.93 M | 684.0× | 异步执行器 fire-and-forget（与链共用同一线程池） |
| asio::post (1 thread) | 5.10 μs | 5.27 μs | 7.68 μs | 7.68 μs | 1.35 μs | 196.27 K | 6709.4× | 行业标准异步库（本项目自带） |

## 2. 异步 promise（CPromise 层开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.8 ns | 0.8 ns | 0.9 ns | 1.1 ns | 0.0 ns | 1.28 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.8 ns | 0.8 ns | 0.9 ns | 1.1 ns | 0.0 ns | 1.21 G | 1.1× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.8 ns | 0.8 ns | 0.9 ns | 1.0 ns | 0.0 ns | 1.20 G | 1.1× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.8 ns | 0.8 ns | 0.8 ns | 1.0 ns | 0.0 ns | 1.29 G | 1.0× | 循环内联，理论下限 |
| CPromise x1 | 763.7 ns | 765.5 ns | 839.0 ns | 881.7 ns | 13.3 ns | 1.31 M | 975.3× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x5 | 4.44 μs | 4.39 μs | 4.76 μs | 5.53 μs | 272.5 ns | 225.14 K | 5672.6× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x20 | 17.26 μs | 17.14 μs | 17.87 μs | 18.72 μs | 576.2 ns | 57.95 K | 22039.4× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x100 | 53.29 μs | 53.54 μs | 57.02 μs | 58.56 μs | 766.3 ns | 18.77 K | 68054.6× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise deep x256 (inline→post) | 117.56 μs | 117.63 μs | 123.89 μs | 130.22 μs | 3.79 μs | 8.51 K | 150143.4× | 深层链：内联 64 层后改投递，验证不爆栈 |
| CPromise fail-fast x20 | 15.11 μs | 15.08 μs | 15.83 μs | 16.13 μs | 465.8 ns | 66.19 K | 19294.9× | 失败后后续层短路（不执行层函数，仅透传结果） |

## 3. 协程（CCoroutine 顺序化）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 ns | 0.8 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.31 G | 1.0× | 直接调用，无调度 |
| CPromise single layer | 798.0 ns | 804.2 ns | 946.0 ns | 946.0 ns | 13.9 ns | 1.25 M | 1048.2× | 起 promise + 单层执行 + Await |
| CCoroutine start+await | 1.38 μs | 1.38 μs | 1.45 μs | 1.45 μs | 43.4 ns | 726.06 K | 1809.1× | CoStart → 1 次 CO_AWAIT（子链）→ 完成 |
| CPromise x10 | 11.79 μs | 11.70 μs | 12.44 μs | 12.44 μs | 467.8 ns | 84.82 K | 15485.9× | 10 层 promise（构建 + 执行 + Await） |
| CCoroutine seq await x10 | 4.44 μs | 4.41 μs | 5.74 μs | 5.74 μs | 194.3 ns | 225.05 K | 5836.5× | 10 次挂起 / 恢复（每次起一条单层子链） |
| CCoroutine parallel await x10 | 5.35 μs | 5.37 μs | 5.58 μs | 5.58 μs | 133.9 ns | 186.79 K | 7032.0× | CO_AWAIT_ALL：10 条子链并行等待，一次恢复 |

## 4. 协程伸缩（长协程 / 批量并发）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine 20 awaits (1 thread) | 7.71 μs | 7.73 μs | 9.61 μs | 11.09 μs | 174.3 ns | 129.68 K | - | 单协程 20 次挂起 / 恢复（每次 await 一条单层子链） |
| batch 200 coro x3 await @1 thread | 514.45 μs | 524.82 μs | 657.29 μs | 657.29 μs | 31.20 μs | 1.94 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @2 thread | 1.71 ms | 1.70 ms | 3.10 ms | 3.10 ms | 91.03 μs | 586.17 | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @4 thread | 3.71 ms | 3.63 ms | 3.92 ms | 3.92 ms | 161.51 μs | 269.89 | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |

## 5. 压力测试（窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 77.61 K | 12.88 | mutex+condvar 线程池 |
| CAsyncExecutor Post (4 threads) | 426.97 K | 2.34 | 异步执行器 fire-and-forget |
| asio::post (4 threads) | 97.43 K | 10.26 | 行业标准异步库 |
| CPromise x4 (4 threads) | 97.58 K | 10.25 | 窗口 1000 条 promise（各 4 层） |
| CPromise x8 (4 threads) | 43.85 K | 22.80 | 窗口 1000 条 promise（各 8 层） |
| CCoroutine x2 await (4 threads) | 65.82 K | 15.19 | 窗口 1000 个协程（各 2 次 await） |
| mixed chain+coro+post (4 producers) | 130.35 K | 7.67 | 4 生产者 × 20000（链 / 协程 / Post 各 1/3） |

## 6. 读写门（同一批任务：读并发 vs 写独占）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call × 256 (baseline) | 10.36 ms | 10.36 ms | 10.50 ms | 10.50 ms | 31.35 μs | 96.49 | 1.0× | 直接顺序执行同一批工作（无调度） |
| PostRead × 256（读：并发） | 2.74 ms | 2.75 ms | 3.21 ms | 3.21 ms | 42.72 μs | 364.95 | 0.3× | 读任务可同时进入（上限 = 执行器线程数） |
| Post × 256（写：独占） | 12.14 ms | 12.17 ms | 14.77 ms | 14.77 ms | 410.21 μs | 82.40 | 1.2× | 写任务互斥（同一执行器内串行） |
