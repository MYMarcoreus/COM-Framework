# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 14:39:39
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 1.0 | 1.35 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 32165.1 | 30015.0 | 55001.0 | 31.09 K | 43509.4× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 3750.9 | 3343.2 | 8133.4 | 266.60 K | 5073.8× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 7411.0 | 7012.1 | 12415.0 | 134.93 K | 10024.8× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 8492.5 | 8252.2 | 16327.7 | 117.75 K | 11487.6× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.8 | 0.7 | 1.8 | 1.23 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.8 | 0.7 | 1.2 | 1.32 G | 0.9× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 | 0.7 | 1.1 | 1.35 G | 0.9× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 | 0.7 | 1.0 | 1.35 G | 0.9× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 24011.0 | 22984.5 | 35630.5 | 41.65 K | 29631.9× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 31427.1 | 24203.3 | 149548.0 | 31.82 K | 38784.1× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 27494.4 | 26281.3 | 36212.3 | 36.37 K | 33930.7× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 51034.4 | 48667.0 | 82339.0 | 19.59 K | 62981.4× | 构建 n 级 Then + 执行 + 取值（合计） |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 0.9 | 1.35 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 24297.0 | 22737.2 | 50865.0 | 41.16 K | 32856.3× | 提交单个任务并取值 |
| CCoroutine start+await+done | 26076.5 | 23612.3 | 72377.7 | 38.35 K | 35262.6× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.9 | 0.9 | 1.1 | 1.09 G | 1.2× | 循环 10 次 |
| CAsyncExecutor chain x10 | 25793.7 | 24576.0 | 41302.7 | 38.77 K | 34880.2× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 27909.9 | 27821.0 | 29005.7 | 35.83 K | 37741.9× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| CCoroutine create x100 | 2425.0 | 2425.0 | 2425.0 | 412.37 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 957.1 | 957.1 | 957.1 | 1.04 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1356.6 | 1356.6 | 1356.6 | 737.14 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 810.3 | 810.3 | 810.3 | 1.23 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1435.8 | 1435.8 | 1435.8 | 696.48 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 1050.1 | 1050.1 | 1050.1 | 952.29 K | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 93.39 K | 10.71 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 92.82 K | 10.77 | 任务链框架 |
| asio::post (4 threads) | 103.41 K | 9.67 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 111.62 K | 8.96 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 28.57 K | 35.00 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 369.84 K | 2.70 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 6.19 M | 0.16 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 395.04 K | 2.53 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.704 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.785 | 200×1ms 慢任务（等待排空队列） |
