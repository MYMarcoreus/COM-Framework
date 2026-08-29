#include "StressCase.h"

#include "cases/Engines.h"
#include "framework/Bench.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace {

/// 对某引擎跑一次窗口式压力测试。
inline void RunStressFor(const std::string& group, const std::string& name,
                         const std::function<void(const std::function<void()>&)>& submit,
                         const std::function<void()>& stop,
                         int window, int ms, const std::string& note)
{
    std::atomic<uint64_t> done(0);
    std::function<void()> wrap = [&]() {
        submit([&done]() { done.fetch_add(1, std::memory_order_release); });
    };
    benchmark::StressWindow(group, name, window, ms, wrap, done, note);
    stop();
}

} // namespace

void RunStressCases()
{
    const std::string group = "4. 压力测试（4 线程，窗口式稳定吞吐）";
    const int kThreads = 4;
    const int kMs = 2000;

    // CThreadPool（4 线程，窗口 500）。
    {
        bench::PoolEngine eng;
        eng.Start(kThreads);
        RunStressFor(group, "CThreadPool (4 threads)",
                     [&eng](const std::function<void()>& f) { eng.Submit(f); },
                     [&eng]() { eng.Stop(); },
                     500, kMs, "mutex+condvar 线程池");
    }

    // CAsyncExecutor（4 线程，窗口 500）。
    {
        bench::AsyncEngine eng;
        eng.Start(kThreads);
        RunStressFor(group, "CAsyncExecutor (4 threads)",
                     [&eng](const std::function<void()>& f) { eng.Submit(f); },
                     [&eng]() { eng.Stop(); },
                     500, kMs, "任务链框架");
    }

    // asio::post（4 线程，窗口 500）。
    {
        bench::AsioEngine eng;
        eng.Start(kThreads);
        RunStressFor(group, "asio::post (4 threads)",
                     [&eng](const std::function<void()>& f) { eng.Submit(f); },
                     [&eng]() { eng.Stop(); },
                     500, kMs, "行业标准异步库");
    }

    // CAsyncExecutor（4 线程，大窗口 5000，观察背压）。
    {
        bench::AsyncEngine eng;
        eng.Start(kThreads);
        RunStressFor(group, "CAsyncExecutor (4 threads, win 5000)",
                     [&eng](const std::function<void()>& f) { eng.Submit(f); },
                     [&eng]() { eng.Stop(); },
                     5000, kMs, "大窗口，观察背压行为");
    }

    // ---------------- 停止延迟 ----------------
    const std::string stopGroup = "5. 停止延迟（队列有未完成任务时 Stop 阻塞耗时）";
    auto slowFn = []() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };

    {
        bench::PoolEngine eng;
        eng.Start(kThreads);
        for (int i = 0; i < 200; ++i)
            eng.Submit(slowFn); // 先塞满队列，不等完成立即 Stop。
        double a = benchmark::NowNs();
        eng.Stop();
        double b = benchmark::NowNs();

        benchmark::Result r;
        r.group = stopGroup;
        r.name = "CThreadPool";
        r.mean_ns = (b - a);
        r.ops_per_sec = 0.0;
        r.note = "200×1ms 慢任务（等待排空队列）";
        r.is_stress = true;
        benchmark::Registry::Instance().Add(r);
    }

    {
        bench::AsyncEngine eng;
        eng.Start(kThreads);
        for (int i = 0; i < 200; ++i)
            eng.Submit(slowFn);
        double a = benchmark::NowNs();
        eng.Stop();
        double b = benchmark::NowNs();

        benchmark::Result r;
        r.group = stopGroup;
        r.name = "CAsyncExecutor";
        r.mean_ns = (b - a);
        r.ops_per_sec = 0.0;
        r.note = "200×1ms 慢任务（等待排空队列）";
        r.is_stress = true;
        benchmark::Registry::Instance().Add(r);
    }
}
