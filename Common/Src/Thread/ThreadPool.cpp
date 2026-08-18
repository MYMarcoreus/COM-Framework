#include "Thread/ThreadPool.h"

namespace common {

/// @brief 创建线程池。
///
/// @param threadCount 工作线程数量。
ThreadPool::ThreadPool(size_t threadCount)
    : threadCount_(threadCount), running_(false), stopping_(false)
{
}

/// @brief 销毁线程池。
ThreadPool::~ThreadPool()
{
    Stop();
}

/// @brief 启动工作线程。
bool ThreadPool::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
    {
        return false;
    }
    if (threadCount_ == 0)
    {
        return false;
    }
    running_ = true;
    stopping_ = false;
    for (size_t i = 0; i < threadCount_; ++i)
    {
        workers_.push_back(std::thread(&ThreadPool::WorkerLoop, this));
    }
    return true;
}

/// @brief 提交任务。
///
/// 将任务加入队列并唤醒一个空闲工作线程。
bool ThreadPool::Submit(const Task& task)
{
    if (!task)
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || stopping_)
        {
            return false;
        }
        tasks_.push_back(task);
    }
    condition_.notify_one();
    return true;
}

/// @brief 停止线程池。
///
/// 通知所有工作线程退出并等待其结束。
void ThreadPool::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_)
        {
            return;
        }
        stopping_ = true;
    }
    condition_.notify_all();
    for (size_t i = 0; i < workers_.size(); ++i)
    {
        if (workers_[i].joinable())
        {
            workers_[i].join();
        }
    }
    workers_.clear();
    running_ = false;
    stopping_ = false;
}

/// @brief 返回工作线程数量。
size_t ThreadPool::ThreadCount() const
{
    return threadCount_;
}

/// @brief 是否正在运行。
bool ThreadPool::IsRunning() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

/// @brief 工作线程循环。
void ThreadPool::WorkerLoop()
{
    while (true)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty())
            {
                break;
            }
            task = tasks_.front();
            tasks_.pop_front();
        }
        if (task)
        {
            task();
        }
    }
}

} // namespace common
