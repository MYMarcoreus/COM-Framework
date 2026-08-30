# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 16:13:44
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.09 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 30.74 μs | 30.56 μs | 32.61 μs | 32.61 μs | 780.6 ns | 32.53 K | 33372.3× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 333.2 ns | 331.3 ns | 343.9 ns | 343.9 ns | 8.2 ns | 3.00 M | 361.7× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 302.4 ns | 301.8 ns | 355.1 ns | 355.1 ns | 4.6 ns | 3.31 M | 328.3× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 3.51 μs | 3.54 μs | 8.21 μs | 8.21 μs | 104.3 ns | 285.15 K | 3807.2× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.9 ns | 0.0 ns | 1.35 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 ns | 0.7 ns | 0.7 ns | 0.9 ns | 0.0 ns | 1.37 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 1.5 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 708.8 ns | 707.4 ns | 769.5 ns | 910.0 ns | 24.3 ns | 1.41 M | 958.8× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 3.17 μs | 3.19 μs | 3.69 μs | 4.60 μs | 305.9 ns | 315.81 K | 4283.6× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 11.94 μs | 11.89 μs | 12.47 μs | 13.31 μs | 344.0 ns | 83.74 K | 16156.1× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 39.52 μs | 39.42 μs | 41.75 μs | 47.12 μs | 1.63 μs | 25.30 K | 53468.4× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor flatMap chain x1 | 1.26 μs | 1.25 μs | 1.34 μs | 1.34 μs | 40.6 ns | 792.19 K | 1707.7× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x5 | 4.82 μs | 4.87 μs | 5.29 μs | 5.61 μs | 309.9 ns | 207.32 K | 6525.3× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x20 | 14.11 μs | 14.12 μs | 14.69 μs | 15.38 μs | 533.8 ns | 70.89 K | 19082.3× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x100 | 47.22 μs | 47.23 μs | 51.31 μs | 57.75 μs | 1.12 μs | 21.18 K | 63875.9× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.34 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 485.3 ns | 487.0 ns | 528.8 ns | 528.8 ns | 3.6 ns | 2.06 M | 652.6× | 提交单个任务并取值 |
| CCoroutine start+await+done | 723.1 ns | 724.0 ns | 768.4 ns | 768.4 ns | 10.7 ns | 1.38 M | 972.5× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.7 ns | 0.7 ns | 0.7 ns | 0.7 ns | 0.0 ns | 1.37 G | 1.0× | 循环 10 次 |
| CAsyncExecutor chain x10 | 5.43 μs | 5.50 μs | 8.16 μs | 8.16 μs | 153.2 ns | 184.33 K | 7296.2× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 2.03 μs | 2.02 μs | 2.07 μs | 2.07 μs | 31.0 ns | 493.36 K | 2726.0× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine create x100 | 3.62 μs | 3.62 μs | 0.0 ns | 3.62 μs | 0.0 ns | 276.50 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 757.8 ns | 757.8 ns | 0.0 ns | 757.8 ns | 0.0 ns | 1.32 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1.00 μs | 1.00 μs | 0.0 ns | 1.00 μs | 0.0 ns | 998.33 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 876.2 ns | 876.2 ns | 0.0 ns | 876.2 ns | 0.0 ns | 1.14 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1.27 μs | 1.27 μs | 0.0 ns | 1.27 μs | 0.0 ns | 790.26 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 1.38 μs | 1.38 μs | 0.0 ns | 1.38 μs | 0.0 ns | 725.95 K | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 185.90 K | 5.38 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 170.66 K | 5.86 | 任务链框架 |
| asio::post (4 threads) | 100.84 K | 9.92 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 422.49 K | 2.37 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 142.53 K | 7.02 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 576.37 K | 1.73 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 9.50 M | 0.11 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 9.82 M | 0.10 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.917 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.521 | 200×1ms 慢任务（等待排空队列） |
