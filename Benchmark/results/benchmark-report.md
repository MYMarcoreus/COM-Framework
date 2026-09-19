# 异步 / 协程库性能测试报告

- 生成时间：2026-09-19 14:25:44
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.35 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 35.73 μs | 35.86 μs | 37.36 μs | 37.36 μs | 1.28 μs | 27.99 K | 48404.0× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 305.8 ns | 305.5 ns | 307.0 ns | 307.0 ns | 1.0 ns | 3.27 M | 414.2× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor Post (1 thread) | 391.4 ns | 397.3 ns | 520.4 ns | 520.4 ns | 34.7 ns | 2.56 M | 530.1× | 异步执行器 fire-and-forget（与链共用同一线程池） |
| asio::post (1 thread) | 10.19 μs | 9.87 μs | 11.75 μs | 11.75 μs | 745.5 ns | 98.09 K | 13809.5× | 行业标准异步库（本项目自带） |

## 2. 异步 promise（CPromise 层开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.35 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 ns | 0.7 ns | 0.7 ns | 0.9 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 ns | 0.7 ns | 0.7 ns | 1.0 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| CPromise x1 | 552.9 ns | 552.5 ns | 575.9 ns | 595.1 ns | 5.9 ns | 1.81 M | 745.2× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x5 | 3.55 μs | 3.54 μs | 3.74 μs | 3.94 μs | 129.1 ns | 282.06 K | 4778.4× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x20 | 15.93 μs | 15.99 μs | 17.37 μs | 20.44 μs | 345.8 ns | 62.76 K | 21475.7× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x100 | 51.13 μs | 51.25 μs | 54.83 μs | 58.09 μs | 581.6 ns | 19.56 K | 68906.0× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise deep x256 (inline→post) | 122.33 μs | 122.39 μs | 123.88 μs | 127.67 μs | 753.9 ns | 8.17 K | 164876.7× | 深层链：内联 64 层后改投递，验证不爆栈 |
| CPromise fail-fast x20 | 15.08 μs | 15.09 μs | 15.71 μs | 16.32 μs | 251.9 ns | 66.31 K | 20327.1× | 失败后后续层短路（不执行层函数，仅透传结果） |

## 3. 协程（CCoroutine 顺序化）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.36 G | 1.0× | 直接调用，无调度 |
| CPromise single layer | 580.4 ns | 577.8 ns | 604.8 ns | 604.8 ns | 6.6 ns | 1.72 M | 791.9× | 起 promise + 单层执行 + Await |
| CCoroutine start+await | 966.2 ns | 966.1 ns | 1.21 μs | 1.21 μs | 17.0 ns | 1.04 M | 1318.3× | CoStart → 1 次 CO_AWAIT（子链）→ 完成 |
| CPromise x10 | 8.17 μs | 7.67 μs | 10.38 μs | 10.38 μs | 1.31 μs | 122.39 K | 11147.8× | 10 层 promise（构建 + 执行 + Await） |
| CCoroutine seq await x10 | 2.87 μs | 2.87 μs | 3.30 μs | 3.30 μs | 43.6 ns | 348.13 K | 3919.3× | 10 次挂起 / 恢复（每次起一条单层子链） |
| CCoroutine parallel await x10 | 3.61 μs | 3.59 μs | 3.79 μs | 3.79 μs | 85.0 ns | 277.33 K | 4919.8× | CO_AWAIT_ALL：10 条子链并行等待，一次恢复 |

## 4. 协程伸缩（长协程 / 批量并发）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine 20 awaits (1 thread) | 4.89 μs | 4.88 μs | 4.94 μs | 5.09 μs | 13.5 ns | 204.56 K | - | 单协程 20 次挂起 / 恢复（每次 await 一条单层子链） |
| batch 200 coro x3 await @1 thread | 346.02 μs | 345.24 μs | 350.77 μs | 350.77 μs | 2.02 μs | 2.89 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @2 thread | 746.87 μs | 740.57 μs | 783.21 μs | 783.21 μs | 36.08 μs | 1.34 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @4 thread | 952.14 μs | 965.76 μs | 1.01 ms | 1.01 ms | 30.52 μs | 1.05 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |

## 5. 压力测试（窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 87.03 K | 11.49 | mutex+condvar 线程池 |
| CAsyncExecutor Post (4 threads) | 82.32 K | 12.15 | 异步执行器 fire-and-forget |
| asio::post (4 threads) | 100.86 K | 9.91 | 行业标准异步库 |
| CPromise x4 (4 threads) | 17.34 K | 57.68 | 窗口 1000 条 promise（各 4 层） |
| CPromise x8 (4 threads) | 10.74 K | 93.13 | 窗口 1000 条 promise（各 8 层） |
| CCoroutine x2 await (4 threads) | 280.72 K | 3.56 | 窗口 1000 个协程（各 2 次 await） |
| mixed chain+coro+post (4 producers) | 453.33 K | 2.21 | 4 生产者 × 20000（链 / 协程 / Post 各 1/3） |
