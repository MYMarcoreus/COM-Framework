# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 14:14:03
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 | 0.7 | 2.3 | 1.28 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 43935.0 | 31849.0 | 142846.5 | 22.76 K | 56047.1× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 23197.9 | 22061.0 | 37108.3 | 43.11 K | 29593.2× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 23397.9 | 21968.8 | 42034.2 | 42.74 K | 29848.3× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 23091.6 | 22031.0 | 42666.5 | 43.31 K | 29457.6× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 1.0 | 0.9 | 2.0 | 1.02 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.9 | 0.9 | 1.1 | 1.08 G | 0.9× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 1.0 | 0.9 | 1.7 | 1.05 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 1.0 | 0.9 | 1.8 | 1.05 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 26833.5 | 24913.7 | 56657.7 | 37.27 K | 27327.8× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x5 | 24791.0 | 22985.2 | 51326.0 | 40.34 K | 25247.6× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x20 | 25822.3 | 25121.0 | 30593.0 | 38.73 K | 26297.9× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x100 | 57258.4 | 50667.0 | 137060.0 | 17.46 K | 58313.0× | 构建 n 级 Then + 执行 + 取值 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 1.0 | 1.33 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 23103.9 | 22220.5 | 40763.0 | 43.28 K | 30812.6× | 提交单个任务并取值 |
| CCoroutine start+await+done | 24146.8 | 23040.2 | 44231.8 | 41.41 K | 32203.3× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 1.0 | 0.9 | 2.6 | 1.02 G | 1.3× | 循环 10 次 |
| CAsyncExecutor chain x10 | 26279.3 | 23606.7 | 52596.7 | 38.05 K | 35047.4× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 30300.9 | 27535.3 | 43082.7 | 33.00 K | 40410.8× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| CCoroutine create x100 | 11162.0 | 11162.0 | 11162.0 | 89.59 K | - | CoStart 投递（make_shared + 首 Resume），4 线程 |
| CCoroutine switch x1000 | 1102.8 | 1102.8 | 1102.8 | 906.81 K | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1097.6 | 1097.6 | 1097.6 | 911.05 K | - | CoStart 投递（make_shared + 首 Resume），4 线程 |
| CCoroutine switch x10000 | 814.7 | 814.7 | 814.7 | 1.23 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1168.6 | 1168.6 | 1168.6 | 855.76 K | - | CoStart 投递（make_shared + 首 Resume），4 线程 |
| CCoroutine switch x100000 | 1059.8 | 1059.8 | 1059.8 | 943.58 K | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 101.54 K | 9.85 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 94.15 K | 10.62 | 任务链框架 |
| asio::post (4 threads) | 102.76 K | 9.73 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 97.54 K | 10.25 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 24.82 K | 40.29 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 378.02 K | 2.65 | 并发简单协程：CoStart+1×await+完成 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 54.015 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.498 | 200×1ms 慢任务（等待排空队列） |
