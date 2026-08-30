# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 16:11:33
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 ns | 0.9 ns | 0.9 ns | 0.9 ns | 0.0 ns | 1.08 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 30.16 μs | 30.23 μs | 31.42 μs | 31.42 μs | 361.1 ns | 33.15 K | 32634.4× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 306.2 ns | 307.2 ns | 381.0 ns | 381.0 ns | 6.1 ns | 3.27 M | 331.2× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 306.3 ns | 311.7 ns | 342.4 ns | 342.4 ns | 7.9 ns | 3.27 M | 331.4× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 6.79 μs | 6.87 μs | 7.23 μs | 7.23 μs | 203.8 ns | 147.20 K | 7350.0× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.9 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 1.1 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.9 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 842.3 ns | 835.2 ns | 908.8 ns | 947.5 ns | 30.8 ns | 1.19 M | 1144.6× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 2.49 μs | 2.51 μs | 2.97 μs | 3.89 μs | 133.9 ns | 401.85 K | 3381.6× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 11.65 μs | 11.64 μs | 12.18 μs | 13.07 μs | 180.7 ns | 85.86 K | 15826.8× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 38.36 μs | 38.32 μs | 42.71 μs | 94.82 μs | 1.04 μs | 26.07 K | 52129.5× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor flatMap chain x1 | 1.10 μs | 1.10 μs | 1.16 μs | 1.46 μs | 32.6 ns | 905.42 K | 1500.8× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x5 | 4.64 μs | 4.60 μs | 5.27 μs | 5.50 μs | 285.7 ns | 215.55 K | 6304.3× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x20 | 12.44 μs | 12.40 μs | 13.52 μs | 15.97 μs | 452.0 ns | 80.36 K | 16909.1× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x100 | 46.37 μs | 46.47 μs | 48.35 μs | 51.48 μs | 461.8 ns | 21.56 K | 63016.1× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.34 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 439.5 ns | 435.9 ns | 489.4 ns | 489.4 ns | 9.7 ns | 2.28 M | 590.6× | 提交单个任务并取值 |
| CCoroutine start+await+done | 666.7 ns | 667.9 ns | 701.0 ns | 701.0 ns | 17.3 ns | 1.50 M | 895.8× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.7 ns | 0.7 ns | 0.7 ns | 0.7 ns | 0.0 ns | 1.36 G | 1.0× | 循环 10 次 |
| CAsyncExecutor chain x10 | 7.73 μs | 7.78 μs | 7.93 μs | 7.93 μs | 157.9 ns | 129.41 K | 10382.3× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 1.94 μs | 1.94 μs | 1.97 μs | 1.97 μs | 4.5 ns | 515.06 K | 2608.6× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine create x100 | 3.26 μs | 3.26 μs | 0.0 ns | 3.26 μs | 0.0 ns | 306.70 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 775.5 ns | 775.5 ns | 0.0 ns | 775.5 ns | 0.0 ns | 1.29 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1.25 μs | 1.25 μs | 0.0 ns | 1.25 μs | 0.0 ns | 801.36 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 915.6 ns | 915.6 ns | 0.0 ns | 915.6 ns | 0.0 ns | 1.09 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1.38 μs | 1.38 μs | 0.0 ns | 1.38 μs | 0.0 ns | 725.89 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 995.7 ns | 995.7 ns | 0.0 ns | 995.7 ns | 0.0 ns | 1.00 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 92.20 K | 10.85 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 93.48 K | 10.70 | 任务链框架 |
| asio::post (4 threads) | 100.86 K | 9.91 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 96.37 K | 10.38 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 120.78 K | 8.28 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 518.94 K | 1.93 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 5.86 M | 0.17 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 5.60 M | 0.18 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.776 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.585 | 200×1ms 慢任务（等待排空队列） |
