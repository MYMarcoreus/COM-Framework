// ====================================================================
// 用例：协程伸缩（长协程 / 批量并发）
//   - 长协程：单个协程内 20 次挂起 / 恢复的全程成本（每次 await 的摊销）；
//   - 批量并发：同一载荷（200 个协程 × 3 次 await）在 1 / 2 / 4 线程执行器
//     下的总成本，观察调度伸缩性。
//
// 说明：无栈协程的 Duff's device 状态机用 __LINE__ 作恢复点，宏无法循环展开，
//       故协程体内 await 次数在源码中固定写出。
// ====================================================================
#ifndef COM_BENCHMARK_CASES_RESUMABLECASE_H
#define COM_BENCHMARK_CASES_RESUMABLECASE_H

/// 运行「协程伸缩」基准。
void RunResumableCases();

#endif  // COM_BENCHMARK_CASES_RESUMABLECASE_H
