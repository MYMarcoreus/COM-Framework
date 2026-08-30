# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 16:19:29
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 ns | 0.9 ns | 1.0 ns | 1.0 ns | 0.0 ns | 1.09 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 29.59 μs | 30.31 μs | 40.24 μs | 40.24 μs | 1.52 μs | 33.80 K | 32245.3× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 315.3 ns | 313.8 ns | 348.9 ns | 348.9 ns | 18.6 ns | 3.17 M | 343.6× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 335.0 ns | 344.0 ns | 749.7 ns | 749.7 ns | 18.8 ns | 2.99 M | 365.1× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 7.30 μs | 8.16 μs | 10.01 μs | 10.01 μs | 1.85 μs | 137.00 K | 7954.2× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 1.0 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 1.0 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.9 ns | 0.0 ns | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.37 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 657.1 ns | 656.5 ns | 730.9 ns | 817.6 ns | 28.6 ns | 1.52 M | 891.4× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 3.00 μs | 2.98 μs | 3.21 μs | 3.45 μs | 150.7 ns | 333.75 K | 4064.4× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 11.09 μs | 11.22 μs | 11.93 μs | 12.78 μs | 644.4 ns | 90.15 K | 15047.6× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 37.41 μs | 37.66 μs | 40.90 μs | 54.72 μs | 795.1 ns | 26.73 K | 50752.2× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor flatMap chain x1 | 1.20 μs | 1.22 μs | 1.49 μs | 1.76 μs | 99.1 ns | 831.62 K | 1631.2× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x5 | 4.54 μs | 4.60 μs | 4.86 μs | 5.31 μs | 195.6 ns | 220.39 K | 6154.9× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x20 | 12.69 μs | 12.58 μs | 13.80 μs | 15.75 μs | 430.4 ns | 78.81 K | 17212.6× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x100 | 49.06 μs | 49.13 μs | 52.78 μs | 54.02 μs | 2.03 μs | 20.38 K | 66551.9× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 ns | 0.7 ns | 0.8 ns | 0.8 ns | 0.0 ns | 1.35 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 484.6 ns | 485.9 ns | 619.7 ns | 619.7 ns | 5.9 ns | 2.06 M | 656.0× | 提交单个任务并取值 |
| CCoroutine start+await+done | 682.1 ns | 677.3 ns | 708.9 ns | 708.9 ns | 15.0 ns | 1.47 M | 923.3× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.7 ns | 0.7 ns | 0.7 ns | 0.7 ns | 0.0 ns | 1.37 G | 1.0× | 循环 10 次 |
| CAsyncExecutor chain x10 | 6.96 μs | 7.23 μs | 7.61 μs | 7.61 μs | 388.7 ns | 143.62 K | 9424.9× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 1.96 μs | 1.96 μs | 2.38 μs | 2.38 μs | 16.3 ns | 511.05 K | 2648.7× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值 | P50 | P90 | P99 | MAD | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine create x100 | 4.81 μs | 4.81 μs | 0.0 ns | 4.81 μs | 0.0 ns | 207.83 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 605.5 ns | 605.5 ns | 0.0 ns | 605.5 ns | 0.0 ns | 1.65 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1.48 μs | 1.48 μs | 0.0 ns | 1.48 μs | 0.0 ns | 676.69 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 646.4 ns | 646.4 ns | 0.0 ns | 646.4 ns | 0.0 ns | 1.55 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1.62 μs | 1.62 μs | 0.0 ns | 1.62 μs | 0.0 ns | 618.43 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 892.8 ns | 892.8 ns | 0.0 ns | 892.8 ns | 0.0 ns | 1.12 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 216.76 K | 4.61 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 182.22 K | 5.49 | 任务链框架 |
| asio::post (4 threads) | 104.08 K | 9.61 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 396.06 K | 2.52 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 146.40 K | 6.83 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 577.94 K | 1.73 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 6.36 M | 0.16 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 9.62 M | 0.10 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 54.102 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.818 | 200×1ms 慢任务（等待排空队列） |
