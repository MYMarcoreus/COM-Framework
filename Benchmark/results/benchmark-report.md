# 异步 / 协程库性能测试报告

- 生成时间：2026-09-10 21:11:09
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 0.9 ns | 0.0 ns | 1.07 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 37.74 μs | 38.76 μs | 56.68 μs | 56.68 μs | 1.70 μs | 26.50 K | 40249.7× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 302.5 ns | 303.1 ns | 325.9 ns | 325.9 ns | 5.5 ns | 3.31 M | 322.7× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor Post (1 thread) | 311.1 ns | 320.5 ns | 421.2 ns | 421.2 ns | 15.5 ns | 3.21 M | 331.8× | 异步执行器 fire-and-forget（与链共用同一线程池） |
| asio::post (1 thread) | 7.26 μs | 7.03 μs | 8.82 μs | 8.82 μs | 1.19 μs | 137.66 K | 7748.1× | 行业标准异步库（本项目自带） |

## 2. 异步链（CAsyncChain 层开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.8 ns | 0.8 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.27 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.8 ns | 0.8 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.30 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.8 ns | 0.8 ns | 0.8 ns | 1.1 ns | 0.0 ns | 1.26 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.8 ns | 0.8 ns | 0.8 ns | 0.9 ns | 0.0 ns | 1.27 G | 1.0× | 循环内联，理论下限 |
| CAsyncChain x1 | 569.9 ns | 574.0 ns | 648.0 ns | 693.2 ns | 7.6 ns | 1.75 M | 724.4× | 构建 N 层链 + 首层投递 + 逐层级联 + Get |
| CAsyncChain x5 | 5.76 μs | 5.85 μs | 6.49 μs | 7.24 μs | 409.5 ns | 173.60 K | 7321.6× | 构建 N 层链 + 首层投递 + 逐层级联 + Get |
| CAsyncChain x20 | 17.57 μs | 17.54 μs | 18.24 μs | 20.29 μs | 296.8 ns | 56.90 K | 22336.2× | 构建 N 层链 + 首层投递 + 逐层级联 + Get |
| CAsyncChain x100 | 61.27 μs | 61.57 μs | 63.55 μs | 67.39 μs | 1.31 μs | 16.32 K | 77871.1× | 构建 N 层链 + 首层投递 + 逐层级联 + Get |
| CAsyncChain deep x256 (inline→post) | 149.15 μs | 148.31 μs | 157.91 μs | 167.29 μs | 6.91 μs | 6.70 K | 189567.3× | 深层链：内联 64 层后改投递，验证不爆栈 |
| CAsyncChain fail-fast x20 | 17.00 μs | 17.24 μs | 19.72 μs | 21.59 μs | 670.5 ns | 58.84 K | 21603.0× | 失败后后续层短路（不执行层函数，仅透传结果） |

## 3. 协程（CCoroutine 顺序化）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 1.0 ns | 1.0 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.05 G | 1.0× | 直接调用，无调度 |
| CAsyncChain single layer | 598.5 ns | 601.9 ns | 661.7 ns | 661.7 ns | 8.8 ns | 1.67 M | 627.2× | 起链 + 单层执行 + Get |
| CCoroutine start+await | 899.8 ns | 879.9 ns | 969.0 ns | 969.0 ns | 34.6 ns | 1.11 M | 942.9× | CoStart → 1 次 CO_AWAIT（子链）→ 完成 |
| CAsyncChain x10 | 11.88 μs | 12.24 μs | 12.82 μs | 12.82 μs | 577.6 ns | 84.21 K | 12445.0× | 10 层链（构建 + 执行 + Get） |
| CCoroutine seq await x10 | 3.08 μs | 3.03 μs | 3.31 μs | 3.31 μs | 147.7 ns | 325.20 K | 3222.4× | 10 次挂起 / 恢复（每次起一条单层子链） |
| CCoroutine parallel await x10 | 3.92 μs | 3.93 μs | 4.38 μs | 4.38 μs | 38.8 ns | 255.23 K | 4105.9× | CO_AWAIT_ALL：10 条子链并行等待，一次恢复 |

## 4. 协程伸缩（长协程 / 批量并发）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine 20 awaits (1 thread) | 5.67 μs | 5.52 μs | 6.47 μs | 7.00 μs | 318.2 ns | 176.28 K | - | 单协程 20 次挂起 / 恢复（每次 await 一条单层子链） |
| batch 200 coro x3 await @1 thread | 340.42 μs | 344.25 μs | 377.95 μs | 377.95 μs | 9.93 μs | 2.94 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @2 thread | 834.27 μs | 798.53 μs | 1.07 ms | 1.07 ms | 111.84 μs | 1.20 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @4 thread | 1.10 ms | 1.09 ms | 1.41 ms | 1.41 ms | 69.84 μs | 911.86 | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |

## 5. 压力测试（窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 76.13 K | 13.14 | mutex+condvar 线程池 |
| CAsyncExecutor Post (4 threads) | 81.19 K | 12.32 | 异步执行器 fire-and-forget |
| asio::post (4 threads) | 109.60 K | 9.12 | 行业标准异步库 |
| CAsyncChain x4 (4 threads) | 16.60 K | 60.24 | 窗口 1000 条链（各 4 层） |
| CAsyncChain x8 (4 threads) | 9.97 K | 100.34 | 窗口 1000 条链（各 8 层） |
| CCoroutine x2 await (4 threads) | 306.81 K | 3.26 | 窗口 1000 个协程（各 2 次 await） |
| mixed chain+coro+post (4 producers) | 495.59 K | 2.02 | 4 生产者 × 20000（链 / 协程 / Post 各 1/3） |
