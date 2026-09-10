// ====================================================================
// 用例：压力测试
//   - 窗口式稳定吞吐：4 线程，对比 CThreadPool / CAsyncExecutor(Post) /
//     asio::post；
//   - 链压力：窗口内持续起 4 层 / 8 层链（每条链自带共享上下文）；
//   - 协程压力：窗口内持续起协程（各 2 次 await）；
//   - 混合负载：链 + 协程 + Post 三路生产者并行。
// ====================================================================
#ifndef COM_BENCHMARK_CASES_STRESSCASE_H
#define COM_BENCHMARK_CASES_STRESSCASE_H

/// 运行「压力测试」。
void RunStressCases();

#endif  // COM_BENCHMARK_CASES_STRESSCASE_H
