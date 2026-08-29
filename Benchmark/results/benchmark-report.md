# 异步 / 协程库性能测试报告

- 生成时间：2026-08-29 22:57:47
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 | 0.7 | 1.0 | 1.32 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 40587.3 | 35772.5 | 69950.0 | 24.64 K | 53764.6× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 23245.3 | 22629.8 | 31120.2 | 43.02 K | 30792.2× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 25141.5 | 22852.3 | 43210.0 | 39.77 K | 33304.1× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 27008.9 | 23836.2 | 103924.2 | 37.02 K | 35777.8× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.9 | 0.9 | 1.0 | 1.08 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 1.0 | 0.9 | 1.3 | 1.05 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.9 | 0.9 | 1.0 | 1.09 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 25596.0 | 24784.7 | 36362.3 | 39.07 K | 27608.1× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x5 | 24660.9 | 23574.0 | 45134.3 | 40.55 K | 26599.4× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x20 | 27592.8 | 26010.7 | 37977.0 | 36.24 K | 29761.8× | 构建 n 级 Then + 执行 + 取值 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 0.8 | 1.35 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 24748.6 | 22966.0 | 51168.5 | 40.41 K | 33392.9× | 提交单个任务并取值 |
| CCoroutine start+await+done | 24735.5 | 23846.0 | 30537.0 | 40.43 K | 33375.2× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.9 | 0.9 | 1.1 | 1.07 G | 1.3× | 循环 10 次 |
| CAsyncExecutor chain x10 | 26730.0 | 25349.3 | 32016.7 | 37.41 K | 36066.3× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 30234.3 | 29472.0 | 36985.0 | 33.07 K | 40794.7× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 86.70 K | 11.53 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 84.84 K | 11.79 | 任务链框架 |
| asio::post (4 threads) | 100.29 K | 9.97 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 88.24 K | 11.33 | 大窗口，观察背压行为 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.753 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.887 | 200×1ms 慢任务（等待排空队列） |
