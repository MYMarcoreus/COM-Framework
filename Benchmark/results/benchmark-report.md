# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 14:56:49
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 | 0.9 | 1.4 | 1.07 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 37662.3 | 33282.5 | 62777.5 | 26.55 K | 40436.0× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 7933.2 | 7931.8 | 12812.4 | 126.05 K | 8517.5× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 3650.7 | 3519.0 | 7146.3 | 273.92 K | 3919.6× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 4865.2 | 4740.1 | 7913.7 | 205.54 K | 5223.5× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.7 | 0.7 | 1.0 | 1.34 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.8 | 0.7 | 1.2 | 1.33 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 | 0.7 | 1.1 | 1.34 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.8 | 0.7 | 2.4 | 1.27 G | 1.1× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 12333.0 | 11962.2 | 16915.6 | 81.08 K | 16568.8× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 13339.6 | 12725.0 | 22071.9 | 74.96 K | 17921.1× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 18737.6 | 15706.5 | 40340.3 | 53.37 K | 25173.1× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 45699.1 | 45822.0 | 56950.0 | 21.88 K | 61394.5× | 构建 n 级 Then + 执行 + 取值（合计） |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 0.8 | 1.37 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 11920.6 | 11265.0 | 30771.5 | 83.89 K | 16291.7× | 提交单个任务并取值 |
| CCoroutine start+await+done | 13038.9 | 11932.0 | 30917.3 | 76.69 K | 17820.0× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.9 | 0.9 | 1.0 | 1.09 G | 1.2× | 循环 10 次 |
| CAsyncExecutor chain x10 | 13753.3 | 13919.6 | 15262.7 | 72.71 K | 18796.3× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 15762.4 | 15290.3 | 17587.0 | 63.44 K | 21542.2× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| CCoroutine create x100 | 1901.5 | 1901.5 | 1901.5 | 525.90 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 876.2 | 876.2 | 876.2 | 1.14 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1001.2 | 1001.2 | 1001.2 | 998.80 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 890.5 | 890.5 | 890.5 | 1.12 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1072.6 | 1072.6 | 1072.6 | 932.32 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 921.6 | 921.6 | 921.6 | 1.09 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 93.18 K | 10.73 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 92.41 K | 10.82 | 任务链框架 |
| asio::post (4 threads) | 104.18 K | 9.60 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 115.40 K | 8.67 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 26.23 K | 38.12 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 308.42 K | 3.24 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 6.47 M | 0.15 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 1.83 M | 0.55 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.638 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.493 | 200×1ms 慢任务（等待排空队列） |
