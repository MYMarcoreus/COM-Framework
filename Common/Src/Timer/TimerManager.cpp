#include "Timer/TimerManager.h"

namespace common {

/// @brief 创建定时器管理器。
TimerManager::TimerManager()
    : nextId_(1), running_(false), stopping_(false)
{
}

/// @brief 销毁定时器管理器。
TimerManager::~TimerManager()
{
    Stop();
}

/// @brief 启动定时器线程。
bool TimerManager::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
    {
        return false;
    }
    running_ = true;
    stopping_ = false;
    thread_ = std::thread(&TimerManager::ThreadMain, this);
    return true;
}

/// @brief 添加一次性定时器。
///
/// @param delayMs 延迟毫秒数。
/// @param callback 到期回调。
///
/// @return 定时器标识；失败返回 kInvalidTimerId。
TimerId TimerManager::AddTimer(std::int64_t delayMs, const TimerCallback& callback)
{
    return AddTimerInternal(delayMs, 0, callback);
}

/// @brief 添加周期性定时器。
///
/// @param intervalMs 周期毫秒数。
/// @param callback 每次到期回调。
///
/// @return 定时器标识；失败返回 kInvalidTimerId。
TimerId TimerManager::AddPeriodicTimer(std::int64_t intervalMs, const TimerCallback& callback)
{
    return AddTimerInternal(0, intervalMs, callback);
}

/// @brief 取消定时器。
bool TimerManager::Cancel(TimerId id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<TimerId, Entry*>::iterator it = entries_.find(id);
    if (it == entries_.end())
    {
        return false;
    }
    it->second->canceled = true;
    return true;
}

/// @brief 停止定时器线程。
void TimerManager::Stop()
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
    if (thread_.joinable())
    {
        thread_.join();
    }
    running_ = false;
    stopping_ = false;
}

/// @brief 是否正在运行。
bool TimerManager::IsRunning() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

/// @brief 添加定时器。
///
/// @param delayMs 一次性延迟（intervalMs 为 0 时生效）。
/// @param intervalMs 周期（大于 0 时表示周期性定时器）。
/// @param callback 回调。
TimerId TimerManager::AddTimerInternal(std::int64_t delayMs, std::int64_t intervalMs,
                                       const TimerCallback& callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_)
    {
        return kInvalidTimerId;
    }
    TimerId id = nextId_++;
    Entry* entry = new Entry;
    entry->id = id;
    entry->interval = std::chrono::milliseconds(intervalMs);
    entry->callback = callback;
    entry->canceled = false;
    std::int64_t delay = (intervalMs > 0) ? intervalMs : delayMs;
    entry->expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
    queue_.push(entry);
    entries_[id] = entry;
    condition_.notify_one();
    return id;
}

/// @brief 定时器线程入口。
void TimerManager::ThreadMain()
{
    while (true)
    {
        std::vector<Entry*> due;
        {
            std::unique_lock<std::mutex> lock(mutex_);

            // ① 惰性移除堆顶已取消条目
            while (!queue_.empty() && queue_.top()->canceled)
            {
                Entry* e = queue_.top();
                queue_.pop();
                entries_.erase(e->id);
                delete e;
            }

            // ② 队列为空：等待或退出
            if (queue_.empty())
            {
                if (stopping_)
                {
                    break;
                }
                condition_.wait(lock);
                continue;
            }

            // ③ 等待到最早到期
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            Entry* top = queue_.top();
            if (top->expiry > now)
            {
                condition_.wait_until(lock, top->expiry);
                continue;
            }

            // ④ 取出所有到期条目，锁外触发
            while (!queue_.empty() && queue_.top()->expiry <= now)
            {
                due.push_back(queue_.top());
                queue_.pop();
            }
        }

        // 锁外执行回调，避免回调中再次 AddTimer/Cancel 造成死锁
        for (size_t i = 0; i < due.size(); ++i)
        {
            Entry* entry = due[i];
            if (!entry->canceled && entry->callback)
            {
                entry->callback();
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopping_ && !entry->canceled && entry->interval.count() > 0)
            {
                // 周期性定时器重新入队
                entry->expiry = std::chrono::steady_clock::now() + entry->interval;
                queue_.push(entry);
            }
            else
            {
                entries_.erase(entry->id);
                delete entry;
            }
        }
    }

    // 清理剩余条目
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty())
    {
        Entry* e = queue_.top();
        queue_.pop();
        delete e;
    }
    entries_.clear();
}

} // namespace common
