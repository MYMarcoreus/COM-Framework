// ====================================================================
// 用例：任务链
//   微基准：CAsyncExecutor::Then 链（1 / 5 / 20 级）的「构建 + 执行 +
//   取值」成本，对比等长直接函数链。观察每级 Then 的摊销开销。
// ====================================================================
#ifndef COM_BENCHMARK_CASES_CHAINCASE_H
#define COM_BENCHMARK_CASES_CHAINCASE_H

/// 运行「任务链」基准。
void RunChainCases();

#endif // COM_BENCHMARK_CASES_CHAINCASE_H
