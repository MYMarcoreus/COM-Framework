# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 15:59:37
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.09 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 35.24 μs | 35.04 μs | 37.30 μs | 37.30 μs | 1.83 μs | 28.37 K | 38337.6× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 3.17 μs | 3.21 μs | 5.42 μs | 5.42 μs | 522.6 ns | 315.39 K | 3449.1× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 4.30 μs | 3.95 μs | 7.07 μs | 7.07 μs | 859.4 ns | 232.62 K | 4676.3× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 6.76 μs | 6.80 μs | 9.34 μs | 9.34 μs | 206.5 ns | 148.02 K | 7349.0× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.1 ns | 0.0 ns | 1.09 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.08 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.09 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.2 ns | 0.0 ns | 1.09 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 11.79 μs | 11.74 μs | 12.38 μs | 16.11 μs | 144.8 ns | 84.79 K | 12803.4× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 13.07 μs | 13.15 μs | 14.70 μs | 19.62 μs | 188.2 ns | 76.52 K | 14186.3× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 15.61 μs | 15.63 μs | 17.26 μs | 17.41 μs | 72.2 ns | 64.05 K | 16948.9× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 45.60 μs | 46.24 μs | 52.73 μs | 57.53 μs | 1.99 μs | 21.93 K | 49500.8× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor flatMap chain x1 | 11.98 μs | 12.13 μs | 13.21 μs | 15.99 μs | 533.4 ns | 83.44 K | 13010.2× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x5 | 14.27 μs | 14.25 μs | 15.93 μs | 19.20 μs | 718.6 ns | 70.08 K | 15489.9× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x20 | 19.30 μs | 19.30 μs | 22.41 μs | 29.68 μs | 633.9 ns | 51.80 K | 20956.3× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x100 | 50.75 μs | 50.63 μs | 52.39 μs | 54.90 μs | 811.4 ns | 19.70 K | 55093.8× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.7 ns | 0.7 ns | 0.0 ns | 1.36 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 11.70 μs | 11.71 μs | 12.47 μs | 12.47 μs | 202.5 ns | 85.47 K | 15934.8× | 提交单个任务并取值 |
| CCoroutine start+await+done | 12.71 μs | 12.68 μs | 13.47 μs | 13.47 μs | 155.1 ns | 78.70 K | 17304.5× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.8 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.33 G | 1.0× | 循环 10 次 |
| CAsyncExecutor chain x10 | 14.51 μs | 14.47 μs | 14.92 μs | 14.92 μs | 442.5 ns | 68.94 K | 19754.9× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 15.46 μs | 15.45 μs | 15.90 μs | 15.90 μs | 70.7 ns | 64.68 K | 21057.6× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine create x100 | 1.61 μs | 1.61 μs | 0.0 ns | 1.61 μs | 0.0 ns | 620.44 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 847.5 ns | 847.5 ns | 0.0 ns | 847.5 ns | 0.0 ns | 1.18 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1.77 μs | 1.77 μs | 0.0 ns | 1.77 μs | 0.0 ns | 566.48 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 824.5 ns | 824.5 ns | 0.0 ns | 824.5 ns | 0.0 ns | 1.21 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1.38 μs | 1.38 μs | 0.0 ns | 1.38 μs | 0.0 ns | 724.79 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 935.9 ns | 935.9 ns | 0.0 ns | 935.9 ns | 0.0 ns | 1.07 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 91.01 K | 10.99 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 94.18 K | 10.62 | 任务链框架 |
| asio::post (4 threads) | 99.81 K | 10.02 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 94.31 K | 10.60 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 40.97 K | 24.41 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 288.47 K | 3.47 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 2.59 M | 0.39 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 7.77 M | 0.13 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 54.089 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.885 | 200×1ms 慢任务（等待排空队列） |
