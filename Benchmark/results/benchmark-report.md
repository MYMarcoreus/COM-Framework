# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 13:56:16
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 | 0.9 | 1.4 | 1.07 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 36134.9 | 32646.0 | 52206.0 | 27.67 K | 38738.8× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 23902.4 | 22300.2 | 57584.5 | 41.84 K | 25624.8× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 23017.5 | 22320.8 | 41655.8 | 43.45 K | 24676.1× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 23314.1 | 22439.5 | 43969.0 | 42.89 K | 24994.1× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 | 0.7 | 1.1 | 1.34 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 | 0.7 | 0.8 | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 | 0.7 | 0.8 | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 | 0.7 | 0.8 | 1.36 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 23810.2 | 23122.2 | 34588.2 | 42.00 K | 31910.9× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x5 | 24284.7 | 24163.3 | 26867.3 | 41.18 K | 32546.9× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x20 | 26366.0 | 26150.3 | 29424.7 | 37.93 K | 35336.3× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x100 | 57956.4 | 52915.0 | 116817.0 | 17.25 K | 77674.5× | 构建 n 级 Then + 执行 + 取值 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 | 0.7 | 1.6 | 1.31 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 24766.6 | 23674.7 | 46720.3 | 40.38 K | 32536.4× | 提交单个任务并取值 |
| CCoroutine start+await+done | 23582.3 | 23214.7 | 30500.3 | 42.40 K | 30980.6× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.9 | 0.9 | 1.2 | 1.07 G | 1.2× | 循环 10 次 |
| CAsyncExecutor chain x10 | 25730.1 | 24233.7 | 52657.0 | 38.86 K | 33802.2× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 27532.4 | 27540.3 | 27985.7 | 36.32 K | 36169.8× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 97.60 K | 10.25 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 91.19 K | 10.97 | 任务链框架 |
| asio::post (4 threads) | 104.73 K | 9.55 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 97.15 K | 10.29 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 29.86 K | 33.49 | 每条=Submit+10×Then+完成计数 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.976 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 61.633 | 200×1ms 慢任务（等待排空队列） |
