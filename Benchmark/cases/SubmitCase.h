// ====================================================================
// 用例：任务提交
//   微基准：单任务「提交 → 执行 → 通知 → 唤醒」端到端往返延迟。
//   对比：direct（理论下限）/ std::thread（最重基线）/
//         CThreadPool / CAsyncExecutor / asio::post（第三方基线）。
// ====================================================================
#ifndef COM_BENCHMARK_CASES_SUBMITCASE_H
#define COM_BENCHMARK_CASES_SUBMITCASE_H

/// 运行「任务提交」基准。
void RunSubmitCases();

#endif // COM_BENCHMARK_CASES_SUBMITCASE_H
