#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace common {
namespace thread {

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

    // 提交任务（线程安全，拷贝投递）。
    bool Submit(const CTask& task);

    // 提交任务（线程安全，移动投递：避免 std::function 拷贝）。
    bool Submit(CTask&& task);

    // 停止线程池，等待所有已提交任务执行完毕。
    void Stop();

    // 工作线程数量。
    size_t ThreadCount() const;

    // 是否正在运行。
    bool IsRunning() const;

    // 待处理任务数（队列中未取出的）。
    size_t PendingCount() const;

private:
    // 工作线程循环。
    void WorkerLoop();

    std::vector<std::thread> m_vecWorkers;
    std::deque<CTask> m_dequeTasks;
    std::atomic<long> m_nPending;        // 待处理任务数（Submit +1，WorkerLoop 取出 -1）。
    size_t m_nIdleWorkers;               // 空闲工作线程数（WorkerLoop 维护，Submit 用于按需唤醒）。
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    size_t m_nThreadCount;
    bool m_bRunning;
    bool m_bStopping;
};

} // namespace thread
} // namespace common
