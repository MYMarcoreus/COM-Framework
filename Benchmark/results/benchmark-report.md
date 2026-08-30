# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 14:48:21
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 | 0.7 | 2.3 | 1.21 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 36817.9 | 30346.5 | 109165.5 | 27.16 K | 44493.6× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 8950.1 | 7903.7 | 18494.4 | 111.73 K | 10815.9× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 4566.5 | 4127.2 | 10687.9 | 218.99 K | 5518.5× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 7701.2 | 7485.2 | 17332.3 | 129.85 K | 9306.7× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 1.0 | 0.9 | 2.1 | 1.05 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.9 | 0.9 | 1.4 | 1.08 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.9 | 0.9 | 1.2 | 1.08 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.9 | 0.9 | 1.0 | 1.09 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 12587.2 | 11332.0 | 24979.0 | 79.45 K | 13176.0× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 14420.1 | 12685.0 | 28228.9 | 69.35 K | 15094.7× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 15788.6 | 15498.0 | 19777.2 | 63.34 K | 16527.2× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 49061.0 | 49251.0 | 64088.0 | 20.38 K | 51356.1× | 构建 n 级 Then + 执行 + 取值（合计） |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 | 0.7 | 1.5 | 1.32 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 11951.1 | 11787.9 | 21092.6 | 83.67 K | 15777.8× | 提交单个任务并取值 |
| CCoroutine start+await+done | 14076.6 | 12967.6 | 30860.7 | 71.04 K | 18583.8× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 1.0 | 0.9 | 2.3 | 1.03 G | 1.3× | 循环 10 次 |
| CAsyncExecutor chain x10 | 14842.0 | 14161.5 | 27870.7 | 67.38 K | 19594.3× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 17964.7 | 17366.0 | 31466.2 | 55.66 K | 23716.8× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| CCoroutine create x100 | 2471.0 | 2471.0 | 2471.0 | 404.69 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 956.7 | 956.7 | 956.7 | 1.05 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1097.2 | 1097.2 | 1097.2 | 911.40 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 931.6 | 931.6 | 931.6 | 1.07 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1076.8 | 1076.8 | 1076.8 | 928.68 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 1062.9 | 1062.9 | 1062.9 | 940.85 K | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 90.16 K | 11.09 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 95.53 K | 10.47 | 任务链框架 |
| asio::post (4 threads) | 103.59 K | 9.65 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 102.87 K | 9.72 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 27.53 K | 36.32 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 394.90 K | 2.53 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 2.00 M | 0.50 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 1.41 M | 0.71 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.590 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 58.976 | 200×1ms 慢任务（等待排空队列） |
