# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 15:36:56
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.9 | 0.9 | 1.0 | 1.09 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 40128.5 | 38078.0 | 66572.5 | 24.92 K | 43916.7× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 4581.8 | 4019.0 | 13764.6 | 218.25 K | 5014.4× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 8053.5 | 7889.4 | 13935.5 | 124.17 K | 8813.8× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 7992.9 | 7893.2 | 15249.2 | 125.11 K | 8747.5× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.8 | 0.7 | 1.3 | 1.31 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.7 | 0.7 | 0.9 | 1.35 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.7 | 0.7 | 0.8 | 1.36 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.7 | 0.7 | 0.8 | 1.36 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 10995.8 | 11191.8 | 12355.4 | 90.94 K | 14436.8× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 12322.2 | 12046.4 | 18858.7 | 81.15 K | 16178.3× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 15759.9 | 15880.5 | 19371.8 | 63.45 K | 20691.7× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 40371.2 | 41135.0 | 45441.0 | 24.77 K | 53004.9× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor flatMap chain x1 | 13406.0 | 12250.3 | 23257.9 | 74.59 K | 17601.2× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x5 | 14614.8 | 14157.3 | 17705.2 | 68.42 K | 19188.3× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x20 | 19179.6 | 19064.2 | 23275.4 | 52.14 K | 25181.6× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |
| CAsyncExecutor flatMap chain x100 | 50848.7 | 48604.0 | 59502.0 | 19.67 K | 66761.2× | n 级 flatMap（每级内嵌 Submit），验证 FlatMapForward 路径 |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 1.1 | 1.35 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 12311.3 | 11689.0 | 23064.4 | 81.23 K | 16666.9× | 提交单个任务并取值 |
| CCoroutine start+await+done | 12559.5 | 12188.8 | 25427.1 | 79.62 K | 17002.8× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.7 | 0.7 | 0.8 | 1.37 G | 1.0× | 循环 10 次 |
| CAsyncExecutor chain x10 | 14776.3 | 14055.3 | 30974.9 | 67.68 K | 20003.9× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 15755.5 | 15140.7 | 27542.8 | 63.47 K | 21329.6× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| CCoroutine create x100 | 2235.3 | 2235.3 | 2235.3 | 447.37 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 807.6 | 807.6 | 807.6 | 1.24 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 3783.0 | 3783.0 | 3783.0 | 264.34 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 651.6 | 651.6 | 651.6 | 1.53 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1188.9 | 1188.9 | 1188.9 | 841.10 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 914.1 | 914.1 | 914.1 | 1.09 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 91.26 K | 10.96 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 92.36 K | 10.83 | 任务链框架 |
| asio::post (4 threads) | 100.10 K | 9.99 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 93.16 K | 10.73 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 53.86 K | 18.57 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 379.06 K | 2.64 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 6.02 M | 0.17 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 515.21 K | 1.94 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.640 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 53.614 | 200×1ms 慢任务（等待排空队列） |
