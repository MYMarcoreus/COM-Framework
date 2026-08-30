# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 13:47:01
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 | 0.9 | 1.1 | 1.13 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 28410.1 | 28271.5 | 34643.5 | 35.20 K | 32201.8× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 23294.5 | 21688.0 | 48698.0 | 42.93 K | 26403.5× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 22484.1 | 22027.0 | 32298.2 | 44.48 K | 25484.9× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 23094.4 | 21908.8 | 60977.8 | 43.30 K | 26176.7× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 | 0.7 | 0.8 | 1.43 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 | 0.7 | 0.8 | 1.42 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 | 0.7 | 0.7 | 1.44 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 | 0.7 | 1.0 | 1.43 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 22986.4 | 22586.7 | 26572.0 | 43.50 K | 32900.0× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x5 | 23958.6 | 23344.8 | 35341.5 | 41.74 K | 34291.5× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x20 | 25272.5 | 25009.3 | 28473.7 | 39.57 K | 36172.0× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x100 | 45772.4 | 45294.0 | 52137.0 | 21.85 K | 65513.1× | 构建 n 级 Then + 执行 + 取值 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 0.8 | 1.43 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 22039.8 | 21755.8 | 28127.2 | 45.37 K | 31531.3× | 提交单个任务并取值 |
| CCoroutine start+await+done | 22963.0 | 22485.2 | 28457.0 | 43.55 K | 32852.0× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.9 | 0.9 | 1.0 | 1.15 G | 1.2× | 循环 10 次 |
| CAsyncExecutor chain x10 | 28271.5 | 23393.7 | 68812.0 | 35.37 K | 40446.7× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 27751.6 | 26420.0 | 42960.7 | 36.03 K | 39702.9× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 91.86 K | 10.89 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 90.14 K | 11.09 | 任务链框架 |
| asio::post (4 threads) | 103.80 K | 9.63 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 93.68 K | 10.67 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 24.05 K | 41.58 | 每条=Submit+10×Then+完成计数 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.575 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.373 | 200×1ms 慢任务（等待排空队列） |
