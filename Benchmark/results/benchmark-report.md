# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 16:33:16
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 1.0 ns | 1.0 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.05 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 36.43 μs | 36.67 μs | 42.69 μs | 42.69 μs | 1.91 μs | 27.45 K | 38191.6× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 340.1 ns | 333.3 ns | 365.3 ns | 365.3 ns | 21.0 ns | 2.94 M | 356.6× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 310.5 ns | 312.4 ns | 478.9 ns | 478.9 ns | 5.8 ns | 3.22 M | 325.6× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 5.20 μs | 5.82 μs | 7.15 μs | 7.15 μs | 1.32 μs | 192.46 K | 5447.5× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 ns | 0.7 ns | 0.7 ns | 0.8 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 1.0 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.35 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 ns | 0.7 ns | 0.7 ns | 0.8 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 661.6 ns | 663.3 ns | 698.1 ns | 798.1 ns | 9.8 ns | 1.51 M | 898.3× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 3.11 μs | 3.08 μs | 3.78 μs | 3.90 μs | 207.8 ns | 321.91 K | 4217.8× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 12.28 μs | 12.40 μs | 13.66 μs | 19.41 μs | 878.5 ns | 81.43 K | 16673.1× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 39.68 μs | 39.69 μs | 43.94 μs | 46.77 μs | 793.1 ns | 25.20 K | 53874.5× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor flatMap chain x1 | 1.23 μs | 1.20 μs | 1.46 μs | 1.87 μs | 94.1 ns | 810.26 K | 1675.7× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x5 | 5.25 μs | 5.29 μs | 5.75 μs | 7.07 μs | 287.6 ns | 190.37 K | 7132.1× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x20 | 15.12 μs | 15.24 μs | 16.69 μs | 18.85 μs | 930.6 ns | 66.15 K | 20526.2× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x100 | 50.23 μs | 50.50 μs | 54.63 μs | 59.78 μs | 1.24 μs | 19.91 K | 68202.4× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.35 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 491.5 ns | 481.7 ns | 517.8 ns | 517.8 ns | 13.8 ns | 2.03 M | 665.0× | 提交单个任务并取值 |
| CCoroutine start+await+done | 765.7 ns | 774.4 ns | 810.9 ns | 810.9 ns | 28.8 ns | 1.31 M | 1035.9× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.36 G | 1.0× | 循环 10 次 |
| CAsyncExecutor chain x10 | 7.56 μs | 7.48 μs | 7.86 μs | 7.86 μs | 235.4 ns | 132.26 K | 10229.5× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 2.04 μs | 2.05 μs | 2.22 μs | 2.22 μs | 19.7 ns | 489.40 K | 2764.5× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine create x100 | 2.52 μs | 2.52 μs | 0.0 ns | 2.52 μs | 0.0 ns | 396.25 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 961.3 ns | 961.3 ns | 0.0 ns | 961.3 ns | 0.0 ns | 1.04 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1.47 μs | 1.47 μs | 0.0 ns | 1.47 μs | 0.0 ns | 680.67 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 806.0 ns | 806.0 ns | 0.0 ns | 806.0 ns | 0.0 ns | 1.24 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1.30 μs | 1.30 μs | 0.0 ns | 1.30 μs | 0.0 ns | 770.60 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 955.0 ns | 955.0 ns | 0.0 ns | 955.0 ns | 0.0 ns | 1.05 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 149.81 K | 6.68 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 160.95 K | 6.21 | 任务链框架 |
| asio::post (4 threads) | 100.38 K | 9.96 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 463.79 K | 2.16 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 145.80 K | 6.86 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 573.87 K | 1.74 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 9.29 M | 0.11 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 9.75 M | 0.10 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.792 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.598 | 200×1ms 慢任务（等待排空队列） |
