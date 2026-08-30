# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 13:40:21
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 1.1 | 1.1 | 1.4 | 887.20 M | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 55878.0 | 51832.0 | 102670.0 | 17.90 K | 49575.1× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 29338.1 | 27700.0 | 52488.0 | 34.09 K | 26028.8× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 33480.9 | 32909.3 | 54502.3 | 29.87 K | 29704.3× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 31021.0 | 28513.3 | 87122.7 | 32.24 K | 27521.9× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 1.0 | 1.1 | 1.2 | 979.36 M | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.9 | 0.9 | 1.1 | 1.08 G | 0.9× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 1.0 | 0.9 | 1.3 | 1.04 G | 0.9× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 1.0 | 0.9 | 1.2 | 1.05 G | 0.9× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 30937.4 | 27298.5 | 49896.5 | 32.32 K | 30298.9× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x5 | 32112.9 | 28421.5 | 70377.5 | 31.14 K | 31450.1× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x20 | 32324.0 | 31829.5 | 42850.0 | 30.94 K | 31656.8× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x100 | 71848.7 | 69712.0 | 101215.0 | 13.92 K | 70365.8× | 构建 n 级 Then + 执行 + 取值 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 1.0 | 0.9 | 1.5 | 968.15 M | 1.0× | 直接调用 |
| CAsyncExecutor single task | 29187.2 | 22287.5 | 57678.0 | 34.26 K | 28257.4× | 提交单个任务并取值 |
| CCoroutine start+await+done | 36989.9 | 32804.0 | 67243.5 | 27.03 K | 35811.6× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 1.4 | 1.4 | 2.5 | 722.85 M | 1.3× | 循环 10 次 |
| CAsyncExecutor chain x10 | 49431.2 | 33750.0 | 212950.0 | 20.23 K | 47856.6× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 33447.8 | 31612.0 | 48021.0 | 29.90 K | 32382.3× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 65.59 K | 15.25 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 66.94 K | 14.94 | 任务链框架 |
| asio::post (4 threads) | 75.02 K | 13.33 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 67.27 K | 14.87 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 34.46 K | 29.02 | 每条=Submit+10×Then+完成计数 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 54.240 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 54.276 | 200×1ms 慢任务（等待排空队列） |
