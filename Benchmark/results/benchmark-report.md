# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 15:00:23
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 | 0.9 | 1.1 | 1.11 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 29743.3 | 28658.5 | 49938.5 | 33.62 K | 32875.6× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 4156.7 | 4016.3 | 8117.8 | 240.57 K | 4594.5× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 4140.6 | 3660.9 | 11432.5 | 241.51 K | 4576.7× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 4727.9 | 3726.1 | 12124.7 | 211.51 K | 5225.8× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.8 | 0.7 | 1.8 | 1.32 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.8 | 0.7 | 1.5 | 1.30 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.8 | 0.7 | 1.4 | 1.20 G | 1.1× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.8 | 0.7 | 1.5 | 1.24 G | 1.1× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 10569.0 | 9695.4 | 29985.7 | 94.62 K | 13997.9× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 12128.1 | 11751.5 | 23675.1 | 82.45 K | 16062.8× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 15036.3 | 14599.7 | 19421.5 | 66.51 K | 19914.5× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 49241.0 | 49333.0 | 58294.0 | 20.31 K | 65216.0× | 构建 n 级 Then + 执行 + 取值（合计） |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 1.0 | 1.36 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 11812.7 | 10993.9 | 30028.8 | 84.65 K | 16030.8× | 提交单个任务并取值 |
| CCoroutine start+await+done | 12485.6 | 12229.4 | 15819.1 | 80.09 K | 16943.9× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.9 | 0.9 | 1.0 | 1.09 G | 1.2× | 循环 10 次 |
| CAsyncExecutor chain x10 | 20763.2 | 14324.0 | 57232.2 | 48.16 K | 28177.4× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 16785.1 | 14485.8 | 41938.5 | 59.58 K | 22778.7× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| CCoroutine create x100 | 1931.2 | 1931.2 | 1931.2 | 517.81 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 937.1 | 937.1 | 937.1 | 1.07 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1342.2 | 1342.2 | 1342.2 | 745.05 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 867.1 | 867.1 | 867.1 | 1.15 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1297.8 | 1297.8 | 1297.8 | 770.55 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 956.1 | 956.1 | 956.1 | 1.05 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 95.24 K | 10.50 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 92.47 K | 10.81 | 任务链框架 |
| asio::post (4 threads) | 99.83 K | 10.02 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 100.27 K | 9.97 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 24.43 K | 40.93 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 345.37 K | 2.90 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 2.83 M | 0.35 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 5.59 M | 0.18 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.921 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.678 | 200×1ms 慢任务（等待排空队列） |
