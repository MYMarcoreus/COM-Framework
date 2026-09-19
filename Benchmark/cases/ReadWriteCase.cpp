#include "ReadWriteCase.h"

#include <atomic>
#include <cstdint>
#include <string>

#include "Async/AsyncExecutor.h"
#include "Async/ReadWriteGate.h"
#include "framework/Bench.h"

namespace {

using common::async::CAsyncExecutor;
using common::async::TaskKind;

/// 一次 BenchOp 调用 = 一批任务（比较同一批工作在两种类别下的耗时）。
const int kTasksPerBatch = 256;

/// 任务体空转圈数：**每个任务约 40 µs 业务**（让业务量主导调度开销 ——
/// 线程池在「队列空窗」下每任务有 ~5~15 µs 的睡眠/唤醒成本，任务太短时测的是调度而不是读写门）。
const int kSpinLoops = 200000;

/// @brief 任务体：空转一小会儿（模拟模块内的只读 / 写入工作）。
void SpinWork()
{
    volatile long long nSum = 0;
    for (int i = 0; i < kSpinLoops; ++i)
    {
        nSum += i;
    }
}

/// @brief 跑一批「过门任务」：提交 kTasksPerBatch 个任务并等全部完成。
///
/// 用原子计数忙等（`benchmark::WaitDone`）而不是 promise：避免把 condvar 的抖动算进来，
/// 与其它用例的计时口径一致。
///
/// @param exec 执行器。
/// @param eKind 任务类别（读 = 可并发 / 写 = 独占）。
void RunBatch(CAsyncExecutor& exec, TaskKind eKind)
{
    std::atomic<uint64_t> nDone(0);
    for (int i = 0; i < kTasksPerBatch; ++i)
    {
        exec.Post(eKind,
            [&nDone]()
            {
                SpinWork();
                nDone.fetch_add(1, std::memory_order_release);
            });
    }
    benchmark::WaitDone(nDone, static_cast<uint64_t>(kTasksPerBatch));
}

}  // namespace

void RunReadWriteCases()
{
    const std::string group = "6. 读写门（同一批任务：读并发 vs 写独占）";
    const int kThreads = 4;

    // 基线：同一批工作直接顺序执行（无调度、无并发）。
    benchmark::BenchOp(
        group, "direct_call × 256 (baseline)",
        []()
        {
            for (int i = 0; i < kTasksPerBatch; ++i)
            {
                SpinWork();
            }
        },
        7, "直接顺序执行同一批工作（无调度）");

    CAsyncExecutor exec("bench-rw", kThreads);
    exec.Start();

    // 读：任务之间可并发进入模块（上限 = 执行器线程数）。
    benchmark::BenchOp(
        group, "Post(kRead) × 256（读：并发）",
        [&exec]()
        {
            RunBatch(exec, TaskKind::kRead);
        },
        7, "读任务可同时进入（上限 = 执行器线程数）");

    // 写：独占 —— 同一执行器内串行。
    benchmark::BenchOp(
        group, "Post(kWrite) × 256（写：独占）",
        [&exec]()
        {
            RunBatch(exec, TaskKind::kWrite);
        },
        7, "写任务互斥（同一执行器内串行）");

    exec.Stop();
}
