// ====================================================================
// 用例：协程（common::async::CCoroutine，基于异步链的顺序化写法）
//   微基准：
//     - 单层链（起链 + 执行 + Await）vs 协程「CoStart + 1 次 await」vs 直接调用；
//     - 10 层链 vs 协程 10 次顺序 await vs 协程并行 await（CO_AWAIT_ALL）。
//   观察：协程挂起 / 恢复相对直接调用与链式编排的开销。
// ====================================================================
#ifndef COM_BENCHMARK_CASES_COROUTINECASE_H
#define COM_BENCHMARK_CASES_COROUTINECASE_H

/// 运行「协程」基准。
void RunCoroutineCases();

#endif  // COM_BENCHMARK_CASES_COROUTINECASE_H
