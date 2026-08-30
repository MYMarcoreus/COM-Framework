#include "StressCase.h"

#include "Async/Coroutine.h"
#include "cases/Engines.h"
#include "framework/Bench.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

/// 压力协程：一次 await 后完成（完成时对共享计数 +1）。
/// 用于多线程协程吞吐测试（并发 CoStart 多个独立协程）。
class StressCoro : public common::async::CCoroutine<int>
{
public:
    explicit StressCoro(std::atomic<uint64_t>* pDone)
        : m_pDone(pDone), m_value(0) {}

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_value, []() { return 42; });
        if (m_pDone != nullptr)
            m_pDone->fetch_add(1, std::memory_order_release);
        CO_RETURN(m_value);
        CO_END();
    }

private:
    std::atomic<uint64_t>* m_pDone;
    int m_value;
};

/// 多生产者吞吐：K 个提交线程并行提交 total 个任务，测线程池真实并行吞吐。
/// 消除窗口压测中「单提交线程」瓶颈（单生产者仅反映 Post 速率而非消费能力），
/// 对齐 librf 多生产者/消费者方法。
inline void RunMultiProducer(const std::string& group, const std::string& name,
                             const std::function<void(const std::function<void()>&)>& submit,
                             const std::function<void()>& stop,
                             int producers, uint64_t total, const std::string& note)
{
    // 预热：提交一小批并等完成（让工作线程进入忙碌状态，丢弃）。
    {
        std::atomic<uint64_t> warm(0);
        for (int i = 0; i < producers; ++i)
            for (int k = 0; k < 2000; ++k)
                submit([&warm]() { warm.fetch_add(1, std::memory_order_relaxed); });
        benchmark::WaitDone(warm, static_cast<uint64_t>(producers) * 2000);
    }

    std::atomic<uint64_t> done(0);
    const uint64_t perThread = total / static_cast<uint64_t>(producers);
    double t0 = benchmark::NowNs();
    std::vector<std::thread> ths;
    ths.reserve(static_cast<size_t>(producers));
    for (int i = 0; i < producers; ++i)
    {
        ths.emplace_back([&, perThread]() {
            for (uint64_t k = 0; k < perThread; ++k)
                submit([&done]() { done.fetch_add(1, std::memory_order_release); });
        });
    }
    for (size_t i = 0; i < ths.size(); ++i)
        ths[i].join();
    benchmark::WaitDone(done, perThread * static_cast<uint64_t>(producers));
    double t1 = benchmark::NowNs();
    stop();

    const double ops = (t1 - t0) > 0.0
        ? (perThread * static_cast<uint64_t>(producers)) / ((t1 - t0) / 1e9)
        : 0.0;
    benchmark::Result r;
    r.group = group;
    r.name = name;
    r.mean_ns = ops > 0.0 ? 1e9 / ops : 0.0;
    r.ops_per_sec = ops;
    r.note = note;
    r.is_stress = true;
    benchmark::Registry::Instance().Add(r);
}

} // namespace

void RunStressCases()
{
    const std::string group = "4. 压力测试（4 线程，窗口式稳定吞吐）";
    const int kThreads = 4;
    const int kMs = 4000;

    // CThreadPool（4 线程，窗口 500）。
    {
        bench::PoolEngine eng;
        eng.Start(kThreads);
        RunStressFor(group, "CThreadPool (4 threads)",
                     [&eng](const std::function<void()>& f) { eng.Submit(f); },
                     [&eng]() { eng.Stop(); },
                     1000, kMs, "mutex+condvar 线程池");
    }

    // CAsyncExecutor（4 线程，窗口 500）。
    {
        bench::AsyncEngine eng;
        eng.Start(kThreads);
        RunStressFor(group, "CAsyncExecutor (4 threads)",
                     [&eng](const std::function<void()>& f) { eng.Submit(f); },
                     [&eng]() { eng.Stop(); },
                     1000, kMs, "任务链框架");
    }

    // asio::post（4 线程，窗口 500）。
    {
        bench::AsioEngine eng;
        eng.Start(kThreads);
        RunStressFor(group, "asio::post (4 threads)",
                     [&eng](const std::function<void()>& f) { eng.Submit(f); },
                     [&eng]() { eng.Stop(); },
                     1000, kMs, "行业标准异步库");
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

    // CAsyncExecutor：10 级 Then 链吞吐（4 线程，验证链级间内联调度）。
    // 每条"任务"= Submit 初始 + 10×Then 变换 + 完成计数；与单任务吞吐对比，
    // 可观察链级间是否发生跨线程投递（内联后应接近单任务 / 11）。
    {
        bench::AsyncEngine eng;
        eng.Start(kThreads);
        std::atomic<uint64_t> done(0);
        std::function<void()> wrap = [&]() {
            common::async::CTask<int> task = eng.exec->Submit([]() { return 0; });
            for (int k = 0; k < 10; ++k)
                task = task.Then([](int x) { return x + 1; });
            task.OnSuccess([&done](const int&) { done.fetch_add(1, std::memory_order_release); });
            task.OnNone([&done](common::async::detail::CTaskEndReason)
                        { done.fetch_add(1, std::memory_order_release); });
        };
        benchmark::StressWindow(group, "CAsyncExecutor 10-level chain (4 threads)",
                                300, kMs, wrap, done,
                                "每条=Submit+10×Then+完成计数");
        eng.Stop();
    }

    // CCoroutine：多线程协程吞吐（4 线程，并发 CoStart 独立协程）。
    // 验证协程 ResumeInline（内联续接）在多线程下的调度表现；
    // 协程实例相互独立，内联不会破坏并行度（与 Then 链不同）。
    {
        common::async::CAsyncExecutor exec(4);
        exec.Start();
        std::atomic<uint64_t> done(0);
        std::function<void()> wrap = [&]() {
            std::shared_ptr<StressCoro> p = exec.CoStart<StressCoro>(&done);
            (void)p; // 生命周期由框架自持强引用保证，提前释放安全。
        };
        benchmark::StressWindow(group, "CCoroutine (4 threads)",
                                1000, kMs, wrap, done,
                                "并发简单协程：CoStart+1×await+完成");
        exec.Stop();
    }

    // 多生产者吞吐：4 个提交线程并行提交（消除单生产者瓶颈），测真实并行上限。
    {
        bench::PoolEngine eng;
        eng.Start(kThreads);
        RunMultiProducer(group, "CThreadPool (4 producers × 4 threads)",
                         [&eng](const std::function<void()>& f) { eng.Submit(f); },
                         [&eng]() { eng.Stop(); },
                         4, 500000, "4 提交线程并行 × 4 工作线程");
    }
    {
        bench::AsyncEngine eng;
        eng.Start(kThreads);
        RunMultiProducer(group, "CAsyncExecutor (4 producers × 4 threads)",
                         [&eng](const std::function<void()>& f) { eng.Submit(f); },
                         [&eng]() { eng.Stop(); },
                         4, 500000, "4 提交线程并行 × 4 工作线程");
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
