// ====================================================================
// 用例：协程（Common::async::CCoroutine 无栈协程）
//   微基准：
//     - 简单协程「启动 + 一次挂起/恢复 + 完成」vs 等价单任务 vs 直接调用
//     - 10 级链：协程 10 次 CO_AWAIT vs CAsyncExecutor 10 级 Then vs 直接链
//   观察：协程启动 / 挂起 / 恢复相比「直接调用」与「任务框架」的开销。
// ====================================================================
#ifndef COM_BENCHMARK_CASES_COROUTINECASE_H
#define COM_BENCHMARK_CASES_COROUTINECASE_H

/// 运行「协程」基准。
void RunCoroutineCases();

#endif // COM_BENCHMARK_CASES_COROUTINECASE_H
