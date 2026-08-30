# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 13:20:34
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 1.0 | 0.9 | 2.2 | 961.38 M | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 37943.5 | 32460.0 | 74152.0 | 26.36 K | 36478.2× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 24190.8 | 22796.0 | 44934.0 | 41.34 K | 23256.7× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 23747.9 | 23060.3 | 38252.0 | 42.11 K | 22830.9× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 25880.9 | 24485.0 | 36204.0 | 38.64 K | 24881.5× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 1.0 | 0.9 | 1.1 | 1.05 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 1.0 | 0.9 | 1.6 | 1.04 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 1.1 | 0.9 | 2.7 | 936.44 M | 1.1× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 24224.1 | 23804.7 | 31082.3 | 41.28 K | 25476.4× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x5 | 31074.9 | 24914.3 | 74187.7 | 32.18 K | 32681.4× | 构建 n 级 Then + 执行 + 取值 |
| CAsyncExecutor chain x20 | 30689.2 | 26453.7 | 82608.0 | 32.58 K | 32275.8× | 构建 n 级 Then + 执行 + 取值 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 | 0.8 | 1.7 | 1.20 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 29196.5 | 24867.3 | 106563.7 | 34.25 K | 35074.4× | 提交单个任务并取值 |
| CCoroutine start+await+done | 27770.9 | 25137.3 | 102081.7 | 36.01 K | 33361.8× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.9 | 0.9 | 1.1 | 1.06 G | 1.1× | 循环 10 次 |
| CAsyncExecutor chain x10 | 27010.8 | 25524.0 | 49792.7 | 37.02 K | 32448.6× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 31649.3 | 28276.3 | 75757.7 | 31.60 K | 38021.0× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 90.17 K | 11.09 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 84.84 K | 11.79 | 任务链框架 |
| asio::post (4 threads) | 100.33 K | 9.97 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 86.67 K | 11.54 | 大窗口，观察背压行为 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 55.408 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 55.003 | 200×1ms 慢任务（等待排空队列） |
