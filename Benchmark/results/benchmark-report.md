# 异步 / 协程库性能测试报告

- 生成时间：2026-09-19 15:37:18
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 0.9 ns | 0.0 ns | 1.11 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 33.39 μs | 33.22 μs | 33.70 μs | 33.70 μs | 483.0 ns | 29.95 K | 37122.4× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 317.3 ns | 316.6 ns | 338.5 ns | 338.5 ns | 3.6 ns | 3.15 M | 352.8× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor Post (1 thread) | 391.8 ns | 390.2 ns | 431.0 ns | 431.0 ns | 10.7 ns | 2.55 M | 435.7× | 异步执行器 fire-and-forget（与链共用同一线程池） |
| asio::post (1 thread) | 3.85 μs | 3.84 μs | 7.23 μs | 7.23 μs | 617.8 ns | 259.48 K | 4285.2× | 行业标准异步库（本项目自带） |

## 2. 异步 promise（CPromise 层开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.38 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 ns | 0.7 ns | 0.7 ns | 0.8 ns | 0.0 ns | 1.39 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.39 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.39 G | 1.0× | 循环内联，理论下限 |
| CPromise x1 | 589.7 ns | 590.6 ns | 642.4 ns | 737.9 ns | 29.3 ns | 1.70 M | 816.3× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x5 | 3.85 μs | 3.66 μs | 4.65 μs | 4.86 μs | 445.4 ns | 259.97 K | 5325.4× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x20 | 16.84 μs | 16.97 μs | 19.35 μs | 21.24 μs | 826.6 ns | 59.37 K | 23317.2× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x100 | 51.21 μs | 51.43 μs | 55.48 μs | 58.51 μs | 637.7 ns | 19.53 K | 70902.4× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise deep x256 (inline→post) | 116.30 μs | 116.37 μs | 127.12 μs | 130.00 μs | 3.61 μs | 8.60 K | 161016.9× | 深层链：内联 64 层后改投递，验证不爆栈 |
| CPromise fail-fast x20 | 14.10 μs | 14.19 μs | 15.05 μs | 17.11 μs | 413.8 ns | 70.95 K | 19514.1× | 失败后后续层短路（不执行层函数，仅透传结果） |

## 3. 协程（CCoroutine 顺序化）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.7 ns | 0.7 ns | 0.0 ns | 1.40 G | 1.0× | 直接调用，无调度 |
| CPromise single layer | 557.6 ns | 558.3 ns | 610.7 ns | 610.7 ns | 4.1 ns | 1.79 M | 781.4× | 起 promise + 单层执行 + Await |
| CCoroutine start+await | 903.7 ns | 914.9 ns | 1.02 μs | 1.02 μs | 13.7 ns | 1.11 M | 1266.3× | CoStart → 1 次 CO_AWAIT（子链）→ 完成 |
| CPromise x10 | 9.63 μs | 9.64 μs | 9.99 μs | 9.99 μs | 201.5 ns | 103.79 K | 13501.5× | 10 层 promise（构建 + 执行 + Await） |
| CCoroutine seq await x10 | 2.81 μs | 2.81 μs | 2.87 μs | 2.87 μs | 13.4 ns | 355.60 K | 3940.7× | 10 次挂起 / 恢复（每次起一条单层子链） |
| CCoroutine parallel await x10 | 3.57 μs | 3.56 μs | 3.61 μs | 3.61 μs | 39.4 ns | 280.21 K | 5000.9× | CO_AWAIT_ALL：10 条子链并行等待，一次恢复 |

## 4. 协程伸缩（长协程 / 批量并发）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine 20 awaits (1 thread) | 4.99 μs | 4.96 μs | 5.29 μs | 5.97 μs | 64.5 ns | 200.59 K | - | 单协程 20 次挂起 / 恢复（每次 await 一条单层子链） |
| batch 200 coro x3 await @1 thread | 338.34 μs | 336.36 μs | 454.01 μs | 454.01 μs | 8.40 μs | 2.96 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @2 thread | 777.08 μs | 779.97 μs | 836.23 μs | 836.23 μs | 21.73 μs | 1.29 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @4 thread | 864.12 μs | 869.21 μs | 955.78 μs | 955.78 μs | 16.74 μs | 1.16 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |

## 5. 压力测试（窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 103.07 K | 9.70 | mutex+condvar 线程池 |
| CAsyncExecutor Post (4 threads) | 84.56 K | 11.83 | 异步执行器 fire-and-forget |
| asio::post (4 threads) | 108.01 K | 9.26 | 行业标准异步库 |
| CPromise x4 (4 threads) | 21.51 K | 46.50 | 窗口 1000 条 promise（各 4 层） |
| CPromise x8 (4 threads) | 12.43 K | 80.47 | 窗口 1000 条 promise（各 8 层） |
| CCoroutine x2 await (4 threads) | 282.77 K | 3.54 | 窗口 1000 个协程（各 2 次 await） |
| mixed chain+coro+post (4 producers) | 480.83 K | 2.08 | 4 生产者 × 20000（链 / 协程 / Post 各 1/3） |
