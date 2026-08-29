#include "SubmitCase.h"

#include "cases/Engines.h"
#include "framework/Bench.h"

#include <future>
#include <string>
#include <thread>

namespace {

/// 通过 promise 等待单任务完成（对三种异步引擎统一语义）。
template <typename TEngine>
inline void RunOneWithPromise(TEngine& eng, const std::function<void()>& work)
{
    std::promise<void> p;
    eng.Submit([&p, &work]() {
        work();
        p.set_value();
    });
    p.get_future().wait();
}

} // namespace

void RunSubmitCases()
{
    const std::string group = "1. 任务提交（单任务端到端往返）";
    const int kThreads = 1;

    // 基线：直接函数调用（理论下限）。
    benchmark::BenchOp(group, "direct_call (baseline)",
        []() { volatile int s = 42; (void)s; }, 41, "直接调用，无调度");

    // 最重基线：每任务新建线程 + join（线程创建成本参考）。
    benchmark::BenchOp(group, "std::thread (per-task)",
        []() { std::thread t([]() {}); t.join(); }, 21, "每任务创建线程，无复用");

    // CThreadPool（1 工作线程）。
    {
        bench::PoolEngine eng;
        eng.Start(kThreads);
        benchmark::BenchOp(group, "CThreadPool (1 thread)",
            [&eng]() { RunOneWithPromise(eng, []() {}); },
            41, "mutex+condvar 线程池，提交→执行→唤醒");
        eng.Stop();
    }

    // CAsyncExecutor（1 工作线程）。
    {
        bench::AsyncEngine eng;
        eng.Start(kThreads);
        benchmark::BenchOp(group, "CAsyncExecutor (1 thread)",
            [&eng]() { RunOneWithPromise(eng, []() {}); },
            41, "任务链框架（Option 风格），提交→执行→唤醒");
        eng.Stop();
    }

    // asio::post（第三方基线）。
    {
        bench::AsioEngine eng;
        eng.Start(kThreads);
        benchmark::BenchOp(group, "asio::post (1 thread)",
            [&eng]() { RunOneWithPromise(eng, []() {}); },
            41, "行业标准异步库（本项目自带）");
        eng.Stop();
    }
}
