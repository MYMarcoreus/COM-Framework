#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace common {

/// @brief 无效定时器标识。
static const std::uint64_t kInvalidTimerId = 0;

/// @brief 定时器标识。
using TimerId = std::uint64_t;

/// @brief 定时器回调。
using TimerCallback = std::function<void()>;

/// @brief 定时器管理器。
///
/// 在独立线程运行，支持一次性与周期性定时器。
/// 定时器回调在管理器线程内执行，应尽快返回。
class TimerManager
{
public:
    TimerManager();

    ~TimerManager();

    // 启动定时器线程。
    bool Start();

    // 添加一次性定时器（delayMs 毫秒后触发一次）。
    TimerId AddTimer(std::int64_t delayMs, const TimerCallback& callback);

    // 添加周期性定时器（每 intervalMs 毫秒触发一次）。
    TimerId AddPeriodicTimer(std::int64_t intervalMs, const TimerCallback& callback);

    // 取消定时器。
    bool Cancel(TimerId id);

    // 停止定时器线程。
    void Stop();

    // 是否正在运行。
    bool IsRunning() const;

private:
    struct Entry
    {
        TimerId id;
        std::chrono::steady_clock::time_point expiry;
        std::chrono::milliseconds interval; // 0 表示一次性
        TimerCallback callback;
        bool canceled;
    };

    // 最小堆：最早到期的定时器在堆顶。
    struct EntryCompare
    {
        bool operator()(const Entry* a, const Entry* b) const
        {
            return a->expiry > b->expiry;
        }
    };

    // 添加定时器。
    TimerId AddTimerInternal(std::int64_t delayMs, std::int64_t intervalMs,
                             const TimerCallback& callback);

    // 定时器线程入口。
    void ThreadMain();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::priority_queue<Entry*, std::vector<Entry*>, EntryCompare> queue_;
    std::map<TimerId, Entry*> entries_;
    std::thread thread_;
    TimerId nextId_;
    bool running_;
    bool stopping_;
};

} // namespace common
