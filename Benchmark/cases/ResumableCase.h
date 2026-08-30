// ====================================================================
// 用例：协程创建 / 切换成本（对齐 tearshark/librf 的 resumable_switch 方法）
//
// 借鉴 librf test_async_resumable.cpp 的测试思想：
//   1. 创建与切换分离计时：
//      - create：CoStart N 个协程的「创建 + 投递」成本（ns/op）；
//      - switch：N 个协程内部共 M 次 CO_AWAIT 挂起/恢复的总成本
//        （每次 await 的 ns）。
//   2. 不同协程数量梯度（100 / 1000 / 10000），观察扩展性。
//
// 说明：
//   - 本框架是无栈协程（Duff's device 状态机），宏受 __LINE__ 限制不能
//     循环展开，故每个协程固定展开 10 次 CO_AWAIT；
//   - 线程池后台自动调度（非 librf 显式 run_until_notask），create 阶段
//     会包含少量并行执行，switch 阶段以「新完成切换数」为分母修正。
// ====================================================================
#ifndef COM_BENCHMARK_CASES_RESUMABLECASE_H
#define COM_BENCHMARK_CASES_RESUMABLECASE_H

/// 运行「协程创建/切换」基准。
void RunResumableCases();

#endif // COM_BENCHMARK_CASES_RESUMABLECASE_H
