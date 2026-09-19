# 异步 / 协程库性能测试报告

- 生成时间：2026-09-19 09:57:12
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.7 ns | 0.7 ns | 0.0 ns | 1.35 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 33.61 μs | 34.11 μs | 43.36 μs | 43.36 μs | 1.99 μs | 29.75 K | 45520.2× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 342.2 ns | 347.8 ns | 385.1 ns | 385.1 ns | 24.4 ns | 2.92 M | 463.4× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor Post (1 thread) | 374.4 ns | 374.6 ns | 380.8 ns | 380.8 ns | 4.2 ns | 2.67 M | 507.0× | 异步执行器 fire-and-forget（与链共用同一线程池） |
| asio::post (1 thread) | 7.48 μs | 7.51 μs | 8.16 μs | 8.16 μs | 143.7 ns | 133.62 K | 10134.6× | 行业标准异步库（本项目自带） |

## 2. 异步 promise（CPromise 层开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.35 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 1.1 ns | 0.0 ns | 1.35 G | 1.0× | 循环内联，理论下限 |
| CPromise x1 | 596.0 ns | 600.0 ns | 675.1 ns | 764.5 ns | 23.4 ns | 1.68 M | 806.4× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x5 | 3.86 μs | 3.91 μs | 4.29 μs | 4.41 μs | 148.3 ns | 259.25 K | 5219.1× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x20 | 15.68 μs | 15.41 μs | 17.89 μs | 18.06 μs | 893.6 ns | 63.78 K | 21213.9× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x100 | 51.90 μs | 52.00 μs | 54.65 μs | 58.61 μs | 1.53 μs | 19.27 K | 70221.1× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise deep x256 (inline→post) | 119.39 μs | 121.21 μs | 125.83 μs | 126.56 μs | 4.62 μs | 8.38 K | 161547.6× | 深层链：内联 64 层后改投递，验证不爆栈 |
| CPromise fail-fast x20 | 14.45 μs | 14.48 μs | 16.86 μs | 17.82 μs | 1.60 μs | 69.22 K | 19548.2× | 失败后后续层短路（不执行层函数，仅透传结果） |

## 3. 协程（CCoroutine 顺序化）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.36 G | 1.0× | 直接调用，无调度 |
| CPromise single layer | 603.6 ns | 600.7 ns | 615.3 ns | 615.3 ns | 6.9 ns | 1.66 M | 819.0× | 起 promise + 单层执行 + Await |
| CCoroutine start+await | 988.3 ns | 995.7 ns | 1.15 μs | 1.15 μs | 13.3 ns | 1.01 M | 1341.2× | CoStart → 1 次 CO_AWAIT（子链）→ 完成 |
| CPromise x10 | 10.21 μs | 10.02 μs | 11.26 μs | 11.26 μs | 487.8 ns | 97.98 K | 13850.1× | 10 层 promise（构建 + 执行 + Await） |
| CCoroutine seq await x10 | 3.00 μs | 3.00 μs | 3.34 μs | 3.34 μs | 107.8 ns | 333.87 K | 4064.6× | 10 次挂起 / 恢复（每次起一条单层子链） |
| CCoroutine parallel await x10 | 3.70 μs | 3.73 μs | 4.28 μs | 4.28 μs | 44.7 ns | 270.11 K | 5024.1× | CO_AWAIT_ALL：10 条子链并行等待，一次恢复 |

## 4. 协程伸缩（长协程 / 批量并发）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine 20 awaits (1 thread) | 5.23 μs | 5.22 μs | 5.49 μs | 5.50 μs | 227.1 ns | 191.18 K | - | 单协程 20 次挂起 / 恢复（每次 await 一条单层子链） |
| batch 200 coro x3 await @1 thread | 352.76 μs | 353.28 μs | 391.37 μs | 391.37 μs | 4.83 μs | 2.83 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @2 thread | 808.21 μs | 803.69 μs | 843.41 μs | 843.41 μs | 20.15 μs | 1.24 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @4 thread | 913.49 μs | 911.77 μs | 929.64 μs | 929.64 μs | 15.23 μs | 1.09 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |

## 5. 压力测试（窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 93.94 K | 10.65 | mutex+condvar 线程池 |
| CAsyncExecutor Post (4 threads) | 89.28 K | 11.20 | 异步执行器 fire-and-forget |
| asio::post (4 threads) | 111.36 K | 8.98 | 行业标准异步库 |
| CPromise x4 (4 threads) | 18.29 K | 54.67 | 窗口 1000 条 promise（各 4 层） |
| CPromise x8 (4 threads) | 9.81 K | 101.97 | 窗口 1000 条 promise（各 8 层） |
| CCoroutine x2 await (4 threads) | 283.42 K | 3.53 | 窗口 1000 个协程（各 2 次 await） |
| mixed chain+coro+post (4 producers) | 467.43 K | 2.14 | 4 生产者 × 20000（链 / 协程 / Post 各 1/3） |
