// ====================================================================
// 异步引擎统一封装（供微基准 / 压力测试复用）
//
// 三种 fire-and-forget 引擎，提供一致的 Start / Submit / Stop 接口：
//   - PoolEngine    : Common::thread::CThreadPool（mutex + condvar）
//   - AsyncEngine   : Common::async::CAsyncExecutor（任务链框架）
//   - AsioEngine    : asio::post + io_context（行业标准第三方库，本项目自带）
//
// 注意：Submit 只负责投递；任务完成与否由调用方通过共享原子计数
// （框架的 WaitDone / StressWindow）感知，保证三种引擎语义一致。
// ====================================================================
#ifndef COM_BENCHMARK_CASES_ENGINES_H
#define COM_BENCHMARK_CASES_ENGINES_H

#include "Async/AsyncExecutor.h"
#include "Thread/ThreadPool.h"

#include <asio.hpp>

#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace bench {

/// CThreadPool 引擎。
struct PoolEngine
{
    std::shared_ptr<common::thread::CThreadPool> pool;

    void Start(int n)
    {
        pool.reset(new common::thread::CThreadPool(static_cast<size_t>(n)));
        pool->Start();
    }

    void Submit(const std::function<void()>& f) { pool->Submit(f); }

    void Stop()
    {
        pool->Stop();
        pool.reset();
    }
};

/// CAsyncExecutor 引擎（fire-and-forget 用 Post）。
struct AsyncEngine
{
    std::shared_ptr<common::async::CAsyncExecutor> exec;

    void Start(int n)
    {
        exec.reset(new common::async::CAsyncExecutor(static_cast<size_t>(n)));
        exec->Start();
    }

    void Submit(const std::function<void()>& f) { exec->Post(f); }

    void Stop()
    {
        exec->Stop();
        exec.reset();
    }
};

/// asio 引擎（n 个工作线程同时 run io_context）。
struct AsioEngine
{
    std::shared_ptr<asio::io_context> io;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type> > work;
    std::vector<std::thread> workers;

    void Start(int n)
    {
        io.reset(new asio::io_context());
        work.reset(new asio::executor_work_guard<asio::io_context::executor_type>(
            io->get_executor()));
        workers.clear();
        for (int i = 0; i < n; ++i)
            workers.emplace_back([this]() { io->run(); });
    }

    void Submit(const std::function<void()>& f) { asio::post(*io, f); }

    void Stop()
    {
        work->reset();
        for (size_t i = 0; i < workers.size(); ++i)
            workers[i].join();
        workers.clear();
        io.reset();
        work.reset();
    }
};

} // namespace bench

#endif // COM_BENCHMARK_CASES_ENGINES_H
