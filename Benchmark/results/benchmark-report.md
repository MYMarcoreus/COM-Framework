# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 15:47:39
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P90(ns) | P99(ns) | stddev(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 0.7 | 0.7 | 0.0 | 1.35 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 30875.7 | 30670.2 | 31970.9 | 31970.9 | 965.6 | 32.39 K | 41569.2× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 4316.5 | 4434.5 | 4972.3 | 4972.3 | 467.5 | 231.67 K | 5811.5× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 8786.9 | 8804.2 | 9634.2 | 9634.2 | 617.8 | 113.81 K | 11830.1× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 8108.1 | 8808.6 | 9596.1 | 9596.1 | 1927.7 | 123.33 K | 10916.3× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P90(ns) | P99(ns) | stddev(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.8 | 0.7 | 0.8 | 1.0 | 0.1 | 1.32 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 | 0.7 | 0.8 | 0.8 | 0.0 | 1.34 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 | 0.7 | 0.8 | 0.9 | 0.0 | 1.34 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 | 0.7 | 0.8 | 0.9 | 0.0 | 1.35 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 11360.6 | 11708.6 | 12526.5 | 13150.0 | 1574.4 | 88.02 K | 15020.5× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 12763.0 | 12702.7 | 13142.7 | 14263.3 | 482.4 | 78.35 K | 16874.8× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 15667.2 | 15720.3 | 16451.3 | 16643.9 | 583.4 | 63.83 K | 20714.6× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 40865.4 | 40494.7 | 43158.8 | 45538.5 | 1876.8 | 24.47 K | 54030.7× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor flatMap chain x1 | 12376.1 | 12346.9 | 13012.1 | 14707.9 | 709.6 | 80.80 K | 16363.3× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x5 | 14366.4 | 14205.9 | 15133.5 | 18063.4 | 1148.4 | 69.61 K | 18994.7× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x20 | 19397.8 | 19358.3 | 20090.8 | 21696.2 | 699.7 | 51.55 K | 25647.1× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x100 | 53360.6 | 52732.4 | 55375.7 | 62032.1 | 2906.8 | 18.74 K | 70551.4× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P90(ns) | P99(ns) | stddev(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 | 0.8 | 0.8 | 0.8 | 0.0 | 1.33 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 11499.2 | 11444.9 | 11850.1 | 11850.1 | 258.8 | 86.96 K | 15310.3× | 提交单个任务并取值 |
| CCoroutine start+await+done | 12334.2 | 12177.2 | 13197.2 | 13197.2 | 398.3 | 81.08 K | 16422.1× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.7 | 0.7 | 0.8 | 0.8 | 0.0 | 1.34 G | 1.0× | 循环 10 次 |
| CAsyncExecutor chain x10 | 13712.7 | 13821.8 | 13884.8 | 13884.8 | 203.9 | 72.93 K | 18257.5× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 14947.5 | 14572.4 | 16361.1 | 16361.1 | 805.1 | 66.90 K | 19901.6× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值(ns/op) | P50(ns) | P90(ns) | P99(ns) | stddev(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|---|---|
| CCoroutine create x100 | 2668.1 | 2668.1 | 0.0 | 2668.1 | 0.0 | 374.80 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 871.5 | 871.5 | 0.0 | 871.5 | 0.0 | 1.15 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1492.6 | 1492.6 | 0.0 | 1492.6 | 0.0 | 669.99 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 849.3 | 849.3 | 0.0 | 849.3 | 0.0 | 1.18 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1264.6 | 1264.6 | 0.0 | 1264.6 | 0.0 | 790.77 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 918.3 | 918.3 | 0.0 | 918.3 | 0.0 | 1.09 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 92.83 K | 10.77 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 93.04 K | 10.75 | 任务链框架 |
| asio::post (4 threads) | 103.11 K | 9.70 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 98.63 K | 10.14 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 43.12 K | 23.19 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 300.10 K | 3.33 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 5.88 M | 0.17 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 2.12 M | 0.47 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.921 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.707 | 200×1ms 慢任务（等待排空队列） |
