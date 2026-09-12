#include "StressCase.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"
#include "Async/Promise.h"
#include "cases/ChainContext.h"
#include "cases/Engines.h"
#include "framework/Bench.h"

namespace {

namespace no = common::async;

/// 对某引擎跑一次窗口式压力测试。
inline void RunStressFor(const std::string& group, const std::string& name,
    const std::function<void(const std::function<void()>&)>& submit, const std::function<void()>& stop, int window,
    int ms, const std::string& note)
{
    std::atomic<uint64_t> done(0);
    std::function<void()> wrap = [&]()
    {
        submit(
            [&done]()
            {
                done.fetch_add(1, std::memory_order_release);
            });
    };
    benchmark::StressWindow(group, name, window, ms, wrap, done, note);
    stop();
}

/// promise 压力：窗口内持续起「nLayers 层 promise」并等其完成（每完成一条 done+1）。
inline void RunChainStress(const std::string& group, const std::string& name, int nThreads, int nLayers, int window,
    int ms, const std::string& note)
{
    no::CAsyncExecutor exec(static_cast<size_t>(nThreads));
    exec.Start();

    std::atomic<uint64_t> done(0);
    std::function<void()> startOne = [&exec, &done, nLayers]()
    {
        std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
        no::CPromise<bench::CChainContext> tail = exec.NewPromise(spCtx, &bench::StepInc);
        for (int k = 1; k < nLayers; ++k)
        {
            tail = tail.Then(&bench::StepInc);
        }
        tail.OnSettled(
            [&done](no::CPromiseResult)
            {
                done.fetch_add(1, std::memory_order_release);
            });
    };
    benchmark::StressWindow(group, name, window, ms, startOne, done, note);
    exec.Stop();
}

/// 压力协程：2 次 await 后完成（完成时 AsPromise().OnSettled 计数）。
class StressCoro : public no::CCoroutine<bench::CChainContext>
{
public:
    using no::CCoroutine<bench::CChainContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_AWAIT(NewPromise(&bench::StepInc));
        CO_RETURN_VOID();
        CO_END();
    }
};

/// 协程压力：窗口内持续起协程（每个 2 次 await）并等其完成。
inline void RunCoroStress(
    const std::string& group, const std::string& name, int nThreads, int window, int ms, const std::string& note)
{
    no::CAsyncExecutor exec(static_cast<size_t>(nThreads));
    exec.Start();

    std::atomic<uint64_t> done(0);
    std::function<void()> startOne = [&exec, &done]()
    {
        std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
        std::shared_ptr<StressCoro> pCoro = exec.CoStart<StressCoro>(spCtx);
        // 协程对象由框架自持弱引用保活；这里只挂完成通知用于计数。
        if (!pCoro->AsPromise().OnSettled(
                [&done](no::CPromiseResult)
                {
                    done.fetch_add(1, std::memory_order_release);
                }))
        {
            done.fetch_add(1, std::memory_order_release);  // 注册失败（不应发生）：按已完成计
        }
    };
    benchmark::StressWindow(group, name, window, ms, startOne, done, note);
    exec.Stop();
}

/// 混合负载：链 / 协程 / Post 三路生产者并行，统计总完成吞吐。
inline void RunMixedLoad(const std::string& group, const std::string& name, int nProducers, uint64_t nPerProducer,
    const std::string& note, int nThreads)
{
    no::CAsyncExecutor exec(static_cast<size_t>(nThreads));
    exec.Start();

    std::atomic<uint64_t> done(0);

    // 预热（丢弃）。
    {
        for (int i = 0; i < 500; ++i)
        {
            std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
            exec.NewPromise(spCtx, &bench::StepInc)
                .Then(&bench::StepInc)
                .OnSettled(
                    [&done](no::CPromiseResult)
                    {
                        done.fetch_add(1);
                    });
        }
        benchmark::WaitDone(done, 500);
        done.store(0);
    }

    const double t0 = benchmark::NowNs();
    std::vector<std::thread> threads;
    for (int i = 0; i < nProducers; ++i)
    {
        threads.emplace_back(
            [&, i]()
            {
                for (uint64_t k = 0; k < nPerProducer; ++k)
                {
                    if ((i + static_cast<int>(k)) % 3 == 0)
                    {
                        // 链
                        std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
                        exec.NewPromise(spCtx, &bench::StepInc)
                            .Then(&bench::StepInc)
                            .OnSettled(
                                [&done](no::CPromiseResult)
                                {
                                    done.fetch_add(1);
                                });
                    }
                    else if ((i + static_cast<int>(k)) % 3 == 1)
                    {
                        // 协程
                        std::shared_ptr<bench::CChainContext> spCtx = std::make_shared<bench::CChainContext>();
                        std::shared_ptr<StressCoro> pCoro = exec.CoStart<StressCoro>(spCtx);
                        pCoro->AsPromise().OnSettled(
                            [&done](no::CPromiseResult)
                            {
                                done.fetch_add(1);
                            });
                    }
                    else
                    {
                        // Post（fire-and-forget）
                        exec.Post(
                            [&done]()
                            {
                                done.fetch_add(1);
                            });
                    }
                }
            });
    }
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i].join();
    }
    benchmark::WaitDone(done, nPerProducer * static_cast<uint64_t>(nProducers));
    const double t1 = benchmark::NowNs();
    exec.Stop();

    const uint64_t total = nPerProducer * static_cast<uint64_t>(nProducers);
    const double ops = (t1 - t0) > 0.0 ? total / ((t1 - t0) / 1e9) : 0.0;

    benchmark::Result r;
    r.group = group;
    r.name = name;
    r.mean_ns = ops > 0.0 ? 1e9 / ops : 0.0;
    r.ops_per_sec = ops;
    r.note = note;
    r.is_stress = true;
    benchmark::Registry::Instance().Add(r);
}

}  // namespace

void RunStressCases()
{
    const std::string group = "5. 压力测试（窗口式稳定吞吐）";
    const int kThreads = 4;
    const int kMs = 3000;
    const int kWindow = 1000;

    // 引擎对比：CThreadPool / CAsyncExecutor(Post) / asio::post。
    {
        bench::PoolEngine eng;
        eng.Start(kThreads);
        RunStressFor(
            group, "CThreadPool (4 threads)",
            [&eng](const std::function<void()>& f)
            {
                eng.Submit(f);
            },
            [&eng]()
            {
                eng.Stop();
            },
            kWindow, kMs, "mutex+condvar 线程池");
    }
    {
        bench::AsyncEngine eng;
        eng.Start(kThreads);
        RunStressFor(
            group, "CAsyncExecutor Post (4 threads)",
            [&eng](const std::function<void()>& f)
            {
                eng.Submit(f);
            },
            [&eng]()
            {
                eng.Stop();
            },
            kWindow, kMs, "异步执行器 fire-and-forget");
    }
    {
        bench::AsioEngine eng;
        eng.Start(kThreads);
        RunStressFor(
            group, "asio::post (4 threads)",
            [&eng](const std::function<void()>& f)
            {
                eng.Submit(f);
            },
            [&eng]()
            {
                eng.Stop();
            },
            kWindow, kMs, "行业标准异步库");
    }

    // promise 压力：4 层 / 8 层（每条自带共享上下文，验证分配 + 调度压力）。
    RunChainStress(group, "CPromise x4 (4 threads)", kThreads, 4, kWindow, kMs, "窗口 1000 条 promise（各 4 层）");
    RunChainStress(group, "CPromise x8 (4 threads)", kThreads, 8, kWindow, kMs, "窗口 1000 条 promise（各 8 层）");

    // 协程压力：每个协程 2 次 await。
    RunCoroStress(
        group, "CCoroutine x2 await (4 threads)", kThreads, kWindow, kMs, "窗口 1000 个协程（各 2 次 await）");

    // 混合负载：链 + 协程 + Post 并行。
    RunMixedLoad(group, "mixed chain+coro+post (4 producers)", 4, 20000, "4 生产者 × 20000（链 / 协程 / Post 各 1/3）",
        kThreads);
}
