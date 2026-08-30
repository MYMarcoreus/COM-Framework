# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 14:43:42
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 1.0 | 1.34 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 32553.5 | 30107.0 | 47711.5 | 30.72 K | 43693.0× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 4266.6 | 4005.3 | 12413.9 | 234.38 K | 5726.6× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 8386.8 | 8302.6 | 16591.7 | 119.23 K | 11256.7× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 6753.8 | 6712.9 | 8746.4 | 148.06 K | 9064.9× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.9 | 0.9 | 1.0 | 1.09 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.9 | 0.9 | 1.0 | 1.09 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.9 | 0.9 | 1.3 | 1.07 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.9 | 0.9 | 1.3 | 1.07 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 10848.3 | 10745.0 | 20068.1 | 92.18 K | 11847.2× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 12301.0 | 12146.6 | 15297.9 | 81.29 K | 13433.6× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 14976.5 | 14672.0 | 20609.3 | 66.77 K | 16355.4× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 47058.6 | 48103.5 | 55642.5 | 21.25 K | 51391.4× | 构建 n 级 Then + 执行 + 取值（合计） |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 | 0.7 | 1.1 | 1.31 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 11368.7 | 11516.8 | 15874.6 | 87.96 K | 14894.7× | 提交单个任务并取值 |
| CCoroutine start+await+done | 12648.6 | 12276.0 | 17395.5 | 79.06 K | 16571.5× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.9 | 0.9 | 1.2 | 1.08 G | 1.2× | 循环 10 次 |
| CAsyncExecutor chain x10 | 13920.9 | 13226.6 | 23566.1 | 71.83 K | 18238.4× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 17568.5 | 16829.4 | 22448.6 | 56.92 K | 23017.4× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| CCoroutine create x100 | 2400.6 | 2400.6 | 2400.6 | 416.56 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 1108.3 | 1108.3 | 1108.3 | 902.27 K | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1139.8 | 1139.8 | 1139.8 | 877.36 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 1018.2 | 1018.2 | 1018.2 | 982.14 K | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1052.6 | 1052.6 | 1052.6 | 950.03 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 1086.0 | 1086.0 | 1086.0 | 920.79 K | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 90.67 K | 11.03 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 95.20 K | 10.50 | 任务链框架 |
| asio::post (4 threads) | 100.20 K | 9.98 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 98.89 K | 10.11 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 29.28 K | 34.15 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 377.87 K | 2.65 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 5.68 M | 0.18 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 2.62 M | 0.38 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.779 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.579 | 200×1ms 慢任务（等待排空队列） |
