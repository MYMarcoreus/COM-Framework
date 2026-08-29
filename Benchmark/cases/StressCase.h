// ====================================================================
// 用例：压力测试
//   - 窗口式稳定吞吐：4 线程，窗口 500 任务，持续 2s，
//     对比 CThreadPool / CAsyncExecutor / asio::post。
//   - 大窗口（5000）观察背压行为。
//   - 停止延迟：队列中已有未完成任务时 Stop() 的阻塞耗时。
// ====================================================================
#ifndef COM_BENCHMARK_CASES_STRESSCASE_H
#define COM_BENCHMARK_CASES_STRESSCASE_H

/// 运行「压力测试」。
void RunStressCases();

#endif // COM_BENCHMARK_CASES_STRESSCASE_H
