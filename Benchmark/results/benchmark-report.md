# 异步 / 协程库性能测试报告

- 生成时间：2026-09-19 11:33:31
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.08 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 33.18 μs | 32.99 μs | 34.82 μs | 34.82 μs | 933.2 ns | 30.14 K | 35969.4× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 311.0 ns | 310.7 ns | 340.5 ns | 340.5 ns | 16.9 ns | 3.22 M | 337.2× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor Post (1 thread) | 368.1 ns | 368.1 ns | 432.3 ns | 432.3 ns | 2.4 ns | 2.72 M | 399.0× | 异步执行器 fire-and-forget（与链共用同一线程池） |
| asio::post (1 thread) | 5.41 μs | 4.52 μs | 7.86 μs | 7.86 μs | 1.16 μs | 184.77 K | 5867.7× | 行业标准异步库（本项目自带） |

## 2. 异步 promise（CPromise 层开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 1.0 ns | 0.0 ns | 1.09 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.09 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.1 ns | 0.0 ns | 1.08 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 1.8 ns | 0.0 ns | 1.08 G | 1.0× | 循环内联，理论下限 |
| CPromise x1 | 587.5 ns | 584.2 ns | 630.9 ns | 662.5 ns | 23.2 ns | 1.70 M | 638.4× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x5 | 3.66 μs | 3.66 μs | 4.21 μs | 4.88 μs | 143.6 ns | 272.85 K | 3982.3× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x20 | 16.45 μs | 16.49 μs | 17.61 μs | 18.37 μs | 324.5 ns | 60.78 K | 17877.3× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise x100 | 52.24 μs | 52.03 μs | 53.94 μs | 54.54 μs | 1.10 μs | 19.14 K | 56762.0× | 构建 N 层 promise + 首层投递 + 逐层级联 + Await |
| CPromise deep x256 (inline→post) | 127.02 μs | 127.28 μs | 137.27 μs | 144.37 μs | 8.22 μs | 7.87 K | 138014.9× | 深层链：内联 64 层后改投递，验证不爆栈 |
| CPromise fail-fast x20 | 14.35 μs | 14.30 μs | 16.24 μs | 18.08 μs | 524.4 ns | 69.67 K | 15596.0× | 失败后后续层短路（不执行层函数，仅透传结果） |

## 3. 协程（CCoroutine 顺序化）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.09 G | 1.0× | 直接调用，无调度 |
| CPromise single layer | 596.3 ns | 596.9 ns | 604.0 ns | 604.0 ns | 4.9 ns | 1.68 M | 647.4× | 起 promise + 单层执行 + Await |
| CCoroutine start+await | 939.2 ns | 942.9 ns | 978.3 ns | 978.3 ns | 5.5 ns | 1.06 M | 1019.8× | CoStart → 1 次 CO_AWAIT（子链）→ 完成 |
| CPromise x10 | 10.51 μs | 10.49 μs | 11.61 μs | 11.61 μs | 172.3 ns | 95.13 K | 11412.8× | 10 层 promise（构建 + 执行 + Await） |
| CCoroutine seq await x10 | 2.91 μs | 2.93 μs | 3.09 μs | 3.09 μs | 48.0 ns | 343.77 K | 3158.4× | 10 次挂起 / 恢复（每次起一条单层子链） |
| CCoroutine parallel await x10 | 3.64 μs | 3.66 μs | 3.84 μs | 3.84 μs | 36.2 ns | 274.91 K | 3949.5× | CO_AWAIT_ALL：10 条子链并行等待，一次恢复 |

## 4. 协程伸缩（长协程 / 批量并发）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine 20 awaits (1 thread) | 4.98 μs | 4.98 μs | 5.37 μs | 6.12 μs | 67.4 ns | 200.95 K | - | 单协程 20 次挂起 / 恢复（每次 await 一条单层子链） |
| batch 200 coro x3 await @1 thread | 345.59 μs | 344.87 μs | 351.62 μs | 351.62 μs | 5.39 μs | 2.89 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @2 thread | 740.54 μs | 739.72 μs | 831.91 μs | 831.91 μs | 8.13 μs | 1.35 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |
| batch 200 coro x3 await @4 thread | 949.54 μs | 945.59 μs | 962.91 μs | 962.91 μs | 9.14 μs | 1.05 K | - | 一次逻辑操作 = 200 个协程（各 3 次 await）全部完成 |

## 5. 压力测试（窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 90.00 K | 11.11 | mutex+condvar 线程池 |
| CAsyncExecutor Post (4 threads) | 83.22 K | 12.02 | 异步执行器 fire-and-forget |
| asio::post (4 threads) | 100.36 K | 9.96 | 行业标准异步库 |
| CPromise x4 (4 threads) | 16.30 K | 61.36 | 窗口 1000 条 promise（各 4 层） |
| CPromise x8 (4 threads) | 9.97 K | 100.33 | 窗口 1000 条 promise（各 8 层） |
| CCoroutine x2 await (4 threads) | 284.19 K | 3.52 | 窗口 1000 个协程（各 2 次 await） |
| mixed chain+coro+post (4 producers) | 461.51 K | 2.17 | 4 生产者 × 20000（链 / 协程 / Post 各 1/3） |
