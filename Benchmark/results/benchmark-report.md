# 异步 / 协程库性能测试报告

- 生成时间：2026-09-12 15:21:31
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 0.9 ns | 0.0 ns | 1.08 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 30.67 μs | 30.69 μs | 33.86 μs | 33.86 μs | 197.8 ns | 32.60 K | 33265.1× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 297.2 ns | 299.7 ns | 337.6 ns | 337.6 ns | 12.1 ns | 3.36 M | 322.3× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor Post (1 thread) | 320.4 ns | 322.5 ns | 398.7 ns | 398.7 ns | 5.4 ns | 3.12 M | 347.5× | 异步执行器 fire-and-forget（与链共用同一线程池） |
| asio::post (1 thread) | 8.29 μs | 8.66 μs | 10.64 μs | 10.64 μs | 817.4 ns | 120.62 K | 8991.3× | 行业标准异步库（本项目自带） |

## 2. 异步 promise（CPromise 层开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.1 ns | 0.0 ns | 1.09 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.1 ns | 0.0 ns | 1.09 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.09 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 1.1 ns | 0.0 ns | 1.09 G | 1.0× | 循环内联，理论下限 |
| CPromise x1 | 605.4 ns | 607.4 ns | 836.9 ns | 1.34 μs | 24.9 ns | 1.65 M | 658.6× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x5 | 4.28 μs | 4.33 μs | 4.87 μs | 5.42 μs | 171.1 ns | 233.42 K | 4660.1× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x20 | 17.25 μs | 17.32 μs | 18.25 μs | 19.40 μs | 460.0 ns | 57.96 K | 18767.6× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x100 | 55.94 μs | 55.91 μs | 62.58 μs | 67.57 μs | 995.4 ns | 17.88 K | 60853.5× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise deep x256 (inline→post) | 128.29 μs | 127.39 μs | 134.42 μs | 139.24 μs | 4.91 μs | 7.79 K | 139554.5× | 深层链：内联 64 层后改投递，验证不爆栈 |
| CPromise fail-fast x20 | 16.06 μs | 16.05 μs | 16.60 μs | 18.15 μs | 223.3 ns | 62.25 K | 17475.2× | 失败后后续层短路（不执行层函数，仅透传结果） |

## 3. 协程（CCoroutine 顺序化）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.36 G | 1.0× | 直接调用，无调度 |
| CPromise single layer | 589.8 ns | 590.7 ns | 707.5 ns | 707.5 ns | 5.0 ns | 1.70 M | 804.7× | 起 promise + 单层执行 + Await |
| CCoroutine start+await | 1.00 μs | 1.00 μs | 1.05 μs | 1.05 μs | 10.4 ns | 998.61 K | 1366.4× | CoStart → 1 次 CO_AWAIT（子链）→ 完成 |
| CPromise x10 | 7.81 μs | 7.78 μs | 8.45 μs | 8.45 μs | 310.4 ns | 128.01 K | 10658.7× | 10 层 promise（构建 + 执行 + Await） |
| CCoroutine seq await x10 | 3.31 μs | 3.29 μs | 3.60 μs | 3.60 μs | 151.3 ns | 302.00 K | 4518.1× | 10 次挂起 / 恢复（每次起一条单层子链） |
| CCoroutine parallel await x10 | 4.06 μs | 4.06 μs | 4.23 μs | 4.23 μs | 106.6 ns | 246.28 K | 5540.3× | CO_AWAIT_ALL：10 条子链并行等待，一次恢复 |

## 4. 协程伸缩（长协程 / 批量并发）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine 20 awaits (1 thread) | 5.48 μs | 5.48 μs | 5.53 μs | 5.57 μs | 19.6 ns | 182.53 K | - | 单协程 20 次挂起 / 恢复（每次 await 一条单层子链） |
| batch 200 coro x3 await @1 thread | 347.44 μs | 350.62 μs | 394.48 μs | 394.48 μs | 4.67 μs | 2.88 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @2 thread | 773.22 μs | 773.67 μs | 812.49 μs | 812.49 μs | 20.92 μs | 1.29 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @4 thread | 1.01 ms | 1.01 ms | 1.02 ms | 1.02 ms | 13.26 μs | 993.12 | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |

## 5. 压力测试（窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 90.40 K | 11.06 | mutex+condvar 线程池 |
| CAsyncExecutor Post (4 threads) | 82.88 K | 12.07 | 异步执行器 fire-and-forget |
| asio::post (4 threads) | 102.21 K | 9.78 | 行业标准异步库 |
| CPromise x4 (4 threads) | 18.03 K | 55.45 | 窗口 1000 条 promise（各 4 层） |
| CPromise x8 (4 threads) | 10.78 K | 92.72 | 窗口 1000 条 promise（各 8 层） |
| CCoroutine x2 await (4 threads) | 282.51 K | 3.54 | 窗口 1000 个协程（各 2 次 await） |
| mixed chain+coro+post (4 producers) | 449.96 K | 2.22 | 4 生产者 × 20000（链 / 协程 / Post 各 1/3） |
