# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 14:11:02
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 1.1 | 1.35 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 33750.3 | 30044.0 | 56009.0 | 29.63 K | 45448.1× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 24980.4 | 22308.7 | 50893.7 | 40.03 K | 33638.6× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 23123.5 | 22105.5 | 45051.2 | 43.25 K | 31138.0× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 22970.4 | 22249.0 | 27982.3 | 43.53 K | 30931.8× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.8 | 0.7 | 1.6 | 1.28 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.8 | 0.7 | 1.5 | 1.30 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.8 | 0.7 | 1.7 | 1.29 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 | 0.7 | 1.1 | 1.36 G | 0.9× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 23743.7 | 23065.5 | 29914.0 | 42.12 K | 30364.6× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x5 | 27392.2 | 23999.7 | 67897.0 | 36.51 K | 35030.4× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x20 | 26267.6 | 25646.0 | 34869.0 | 38.07 K | 33592.3× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x100 | 50190.2 | 48463.0 | 58629.0 | 19.92 K | 64185.7× | 构建 n 级 Then + 执行 + 取值 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 1.1 | 1.35 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 23554.5 | 22420.0 | 69172.5 | 42.45 K | 31798.0× | 提交单个任务并取值 |
| CCoroutine start+await+done | 23469.4 | 23297.5 | 25485.5 | 42.61 K | 31683.1× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.9 | 0.9 | 1.7 | 1.05 G | 1.3× | 循环 10 次 |
| CAsyncExecutor chain x10 | 25112.3 | 24420.7 | 30057.3 | 39.82 K | 33900.9× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 30254.9 | 28446.0 | 44601.7 | 33.05 K | 40843.4× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 93.72 K | 10.67 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 93.51 K | 10.69 | 任务链框架 |
| asio::post (4 threads) | 97.41 K | 10.27 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 93.44 K | 10.70 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 29.76 K | 33.60 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 376.82 K | 2.65 | 并发简单协程：CoStart+1×await+完成 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 54.298 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.984 | 200×1ms 慢任务（等待排空队列） |
