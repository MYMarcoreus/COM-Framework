# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 15:26:02
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 1.0 | 0.9 | 3.0 | 1.01 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 142829.1 | 32881.0 | 2281222.0 | 7.00 K | 143653.6× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 3830.3 | 3329.6 | 7800.8 | 261.08 K | 3852.4× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 5574.3 | 4280.0 | 27692.7 | 179.39 K | 5606.5× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 3746.6 | 3406.1 | 8704.6 | 266.91 K | 3768.2× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 | 0.7 | 0.9 | 1.35 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.8 | 0.7 | 1.2 | 1.30 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.8 | 0.7 | 2.7 | 1.25 G | 1.1× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 | 0.7 | 0.9 | 1.35 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 9778.6 | 12080.2 | 14655.2 | 102.26 K | 13235.3× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 6798.5 | 6049.1 | 10367.3 | 147.09 K | 9201.7× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 20670.2 | 15139.8 | 117851.4 | 48.38 K | 27977.1× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 47083.2 | 46829.0 | 61011.0 | 21.24 K | 63726.9× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor flatMap chain x1 | 12301.8 | 11772.3 | 22550.7 | 81.29 K | 16650.4× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x5 | 13441.8 | 13472.4 | 14342.1 | 74.39 K | 18193.4× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x20 | 18720.7 | 18693.6 | 20160.4 | 53.42 K | 25338.4× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x100 | 56626.0 | 54999.0 | 68745.0 | 17.66 K | 76643.0× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 0.8 | 1.36 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 12803.2 | 11752.6 | 37547.3 | 78.11 K | 17439.9× | 提交单个任务并取值 |
| CCoroutine start+await+done | 12344.0 | 12250.2 | 13731.0 | 81.01 K | 16814.4× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 1.0 | 0.9 | 1.4 | 1.04 G | 1.3× | 循环 10 次 |
| CAsyncExecutor chain x10 | 13946.0 | 13868.2 | 17811.2 | 71.70 K | 18996.6× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 14986.5 | 14912.3 | 15503.7 | 66.73 K | 20413.9× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| CCoroutine create x100 | 2272.4 | 2272.4 | 2272.4 | 440.06 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 793.7 | 793.7 | 793.7 | 1.26 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1108.7 | 1108.7 | 1108.7 | 901.92 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 777.0 | 777.0 | 777.0 | 1.29 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1217.6 | 1217.6 | 1217.6 | 821.26 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 939.6 | 939.6 | 939.6 | 1.06 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 96.32 K | 10.38 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 93.05 K | 10.75 | 任务链框架 |
| asio::post (4 threads) | 104.30 K | 9.59 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 96.75 K | 10.34 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 26.41 K | 37.86 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 350.50 K | 2.85 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 2.68 M | 0.37 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 2.10 M | 0.48 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.582 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.941 | 200×1ms 慢任务（等待排空队列） |
