#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace common {

/// @brief 线程池。
///
/// 维护固定数量工作线程与任务队列，提交的任务由空闲线程执行。
/// 基于 C++11 标准库自实现，不依赖第三方库。
class CThreadPool
{
public:
    /// @brief 任务类型。
    using CTask = std::function<void()>;

    // 创建线程池（指定工作线程数）。
    explicit CThreadPool(size_t threadCount = 1);

    ~CThreadPool();

    // 启动工作线程。
    bool Start();

    // 提交任务（线程安全）。
    bool Submit(const CTask& task);

    // 停止线程池，等待所有已提交任务执行完毕。
    void Stop();

    // 工作线程数量。
    size_t ThreadCount() const;

    // 是否正在运行。
    bool IsRunning() const;

private:
    // 工作线程循环。
    void WorkerLoop();

    std::vector<std::thread> workers_;
    std::deque<CTask> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    size_t threadCount_;
    bool running_;
    bool stopping_;
};

} // namespace common
