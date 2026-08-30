# 异步 / 协程库性能测试报告

- 生成时间：2026-08-30 15:18:30
- 环境：Intel(R) Core(TM) i7-14700K（14 核在线）
- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`
- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /
  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）
- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）


## 1. 任务提交（单任务端到端往返）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.8 | 0.7 | 2.2 | 1.28 G | 1.0× | 直接调用，无调度 |
| std::thread (per-task) | 32679.6 | 29690.0 | 61385.5 | 30.60 K | 41820.6× | 每任务创建线程，无复用 |
| CThreadPool (1 thread) | 7254.5 | 7291.2 | 13404.4 | 137.85 K | 9283.7× | mutex+condvar 线程池，提交→执行→唤醒 |
| CAsyncExecutor (1 thread) | 3559.1 | 3382.9 | 6431.4 | 280.97 K | 4554.6× | 任务链框架（Option 风格），提交→执行→唤醒 |
| asio::post (1 thread) | 6076.9 | 5758.5 | 9335.5 | 164.56 K | 7776.7× | 行业标准异步库（本项目自带） |

## 2. 任务链（CAsyncExecutor::Then 开销）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct chain x1 (baseline) | 0.8 | 0.7 | 1.5 | 1.28 G | 1.0× | 循环内联，理论下限 |
| direct chain x5 (baseline) | 0.8 | 0.7 | 1.7 | 1.29 G | 1.0× | 循环内联，理论下限 |
| direct chain x20 (baseline) | 0.8 | 0.7 | 1.6 | 1.31 G | 1.0× | 循环内联，理论下限 |
| direct chain x100 (baseline) | 0.8 | 0.7 | 1.7 | 1.24 G | 1.0× | 循环内联，理论下限 |
| CAsyncExecutor chain x1 | 12505.5 | 11527.6 | 25581.1 | 79.97 K | 15959.7× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x5 | 12751.5 | 12541.9 | 15983.4 | 78.42 K | 16273.6× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x20 | 15517.1 | 15067.0 | 27771.3 | 64.44 K | 19803.2× | 构建 n 级 Then + 执行 + 取值（合计） |
| CAsyncExecutor chain x100 | 53843.9 | 52672.0 | 83962.0 | 18.57 K | 68716.5× | 构建 n 级 Then + 执行 + 取值（合计） |

## 3. 协程（CCoroutine 无栈协程）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| direct_call (baseline) | 0.7 | 0.7 | 0.9 | 1.35 G | 1.0× | 直接调用 |
| CAsyncExecutor single task | 12162.8 | 11631.4 | 21230.3 | 82.22 K | 16470.8× | 提交单个任务并取值 |
| CCoroutine start+await+done | 12462.4 | 12252.4 | 20505.0 | 80.24 K | 16876.4× | CoStart → 一次 CO_AWAIT → CO_RETURN |
| direct chain x10 | 0.9 | 0.9 | 1.9 | 1.06 G | 1.3× | 循环 10 次 |
| CAsyncExecutor chain x10 | 15137.7 | 13901.2 | 32486.0 | 66.06 K | 20499.3× | 10 级 Then 链 |
| CCoroutine chain x10 (10 await) | 15009.7 | 15000.3 | 15863.7 | 66.62 K | 20326.0× | 10 次 CO_AWAIT_INTO 挂起/恢复 |

## 6. 协程创建 / 切换（对齐 librf resumable_switch）

| 实现 | 均值(ns/op) | P50(ns) | P99(ns) | 吞吐(ops/s) | 相对基线 | 说明 |
|---|---|---|---|---|---|---|
| CCoroutine create x100 | 2918.1 | 2918.1 | 2918.1 | 342.69 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x1000 | 765.7 | 765.7 | 765.7 | 1.31 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 100 协程），4 线程 |
| CCoroutine create x1000 | 1175.2 | 1175.2 | 1175.2 | 850.92 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x10000 | 863.5 | 863.5 | 863.5 | 1.16 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 1000 协程），4 线程 |
| CCoroutine create x10000 | 1253.1 | 1253.1 | 1253.1 | 798.01 K | - | CoStart 投递（make_shared + 首 Resume），4 线程；含后台并行执行，小数量（x100）并行不足数值偏高 |
| CCoroutine switch x100000 | 863.0 | 863.0 | 863.0 | 1.16 M | - | 每次 CO_AWAIT 挂起+恢复（10 次/协程 × 10000 协程），4 线程 |

## 4. 压力测试（4 线程，窗口式稳定吞吐）

| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |
|---|---|---|---|
| CThreadPool (4 threads) | 93.84 K | 10.66 | mutex+condvar 线程池 |
| CAsyncExecutor (4 threads) | 87.90 K | 11.38 | 任务链框架 |
| asio::post (4 threads) | 107.95 K | 9.26 | 行业标准异步库 |
| CAsyncExecutor (4 threads, win 5000) | 112.19 K | 8.91 | 大窗口，观察背压行为 |
| CAsyncExecutor 10-level chain (4 threads) | 28.99 K | 34.49 | 每条=Submit+10×Then+完成计数 |
| CCoroutine (4 threads) | 322.97 K | 3.10 | 并发简单协程：CoStart+1×await+完成 |
| CThreadPool (4 producers × 4 threads) | 2.50 M | 0.40 | 4 提交线程并行 × 4 工作线程 |
| CAsyncExecutor (4 producers × 4 threads) | 1.92 M | 0.52 | 4 提交线程并行 × 4 工作线程 |

## 5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）

| 实现 | 阻塞耗时(ms) | 说明 |
|---|---|---|
| CThreadPool | 53.587 | 200×1ms 慢任务（等待排空队列） |
| CAsyncExecutor | 54.165 | 200×1ms 慢任务（等待排空队列） |
