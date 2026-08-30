# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 15:17:05
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 1.0 | 1.35 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 32567.8 | 30145.0 | 48214.0 | 30.71 K | 44027.8× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 5111.1 | 4665.9 | 12748.4 | 195.65 K | 6909.6× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 7616.8 | 7487.3 | 13458.4 | 131.29 K | 10297.0× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 9594.4 | 9070.6 | 20041.6 | 104.23 K | 12970.5× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 | 0.7 | 0.8 | 1.37 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 | 0.7 | 0.9 | 1.35 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 | 0.7 | 0.9 | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 | 0.7 | 0.9 | 1.36 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 12125.3 | 11769.5 | 20081.2 | 82.47 K | 16553.6× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 5888.1 | 5735.1 | 10524.0 | 169.83 K | 8038.5× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 15535.4 | 14830.2 | 24984.0 | 64.37 K | 21209.1× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 49112.0 | 48554.0 | 69250.0 | 20.36 K | 67048.3× | 构建 n 级 Then + 执行 + 取值（合计） |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 | 0.7 | 2.5 | 1.27 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 11579.7 | 11138.2 | 18858.2 | 86.36 K | 14684.9× | 提交单个任务并取值 |
| CCoroutine start+await+done | 11122.5 | 11526.4 | 13704.0 | 89.91 K | 14105.1× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.9 | 0.9 | 1.3 | 1.07 G | 1.2× | 循环 10 次 |
| CAsyncExecutor chain x10 | 14316.0 | 14242.8 | 15127.7 | 69.85 K | 18155.1× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 15270.1 | 15168.3 | 19483.8 | 65.49 K | 19365.0× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| CCoroutine create x100 | 1429.0 | 1429.0 | 1429.0 | 699.80 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 807.3 | 807.3 | 807.3 | 1.24 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1175.0 | 1175.0 | 1175.0 | 851.05 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 874.1 | 874.1 | 874.1 | 1.14 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1240.2 | 1240.2 | 1240.2 | 806.34 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 921.4 | 921.4 | 921.4 | 1.09 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 92.71 K | 10.79 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 89.73 K | 11.14 | 任务链框架 |
| asio::post (4 threads) | 101.46 K | 9.86 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 105.14 K | 9.51 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 25.49 K | 39.23 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 359.42 K | 2.78 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 5.35 M | 0.19 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 1.64 M | 0.61 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.684 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.613 | 200×1ms 慢任务（等待排空队列） |
