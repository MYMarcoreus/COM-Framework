# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 14:31:12
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 1.2 | 1.37 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 33684.4 | 28755.3 | 80272.3 | 29.69 K | 46185.7× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 8205.8 | 7865.2 | 15269.1 | 121.87 K | 11251.2× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 5630.0 | 5673.1 | 7948.6 | 177.62 K | 7719.5× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 6503.3 | 5863.4 | 19540.3 | 153.77 K | 8916.9× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.9 | 0.9 | 1.6 | 1.07 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.9 | 0.9 | 1.9 | 1.07 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.9 | 0.9 | 1.1 | 1.09 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.9 | 0.9 | 1.0 | 1.10 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 23022.9 | 22024.5 | 43631.0 | 43.44 K | 24710.2× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 23808.0 | 22860.5 | 41843.5 | 42.00 K | 25552.8× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 28418.7 | 25563.3 | 81979.3 | 35.19 K | 30501.4× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 60691.7 | 56362.0 | 126825.0 | 16.48 K | 65139.7× | 构建 n 级 Then + 执行 + 取值（合计） |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 0.9 | 1.37 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 25959.5 | 22594.5 | 128784.8 | 38.52 K | 35491.5× | 提交单个任务并取值 |
| CCoroutine start+await+done | 26853.3 | 25523.0 | 61737.7 | 37.24 K | 36713.5× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.9 | 0.9 | 1.1 | 1.10 G | 1.2× | 循环 10 次 |
| CAsyncExecutor chain x10 | 24082.8 | 24069.7 | 24505.3 | 41.52 K | 32925.7× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 27913.8 | 27821.7 | 32724.0 | 35.82 K | 38163.4× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| CCoroutine create x100 | 1840.6 | 1840.6 | 1840.6 | 543.30 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 869.9 | 869.9 | 869.9 | 1.15 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 885.1 | 885.1 | 885.1 | 1.13 M | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 685.6 | 685.6 | 685.6 | 1.46 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1018.4 | 1018.4 | 1018.4 | 981.91 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 1074.6 | 1074.6 | 1074.6 | 930.57 K | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 95.32 K | 10.49 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 93.37 K | 10.71 | 任务链框架 |
| asio::post (4 threads) | 104.26 K | 9.59 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 95.05 K | 10.52 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 31.94 K | 31.31 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 367.66 K | 2.72 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 3.81 M | 0.26 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 221.42 K | 4.52 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 55.738 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.182 | 200×1ms 慢任务（等待排空队列） |
