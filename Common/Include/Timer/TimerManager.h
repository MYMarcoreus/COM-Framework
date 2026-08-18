#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "asio.hpp"

namespace common {

/// @brief 无效定时器标识。
static const std::uint64_t kInvalidTimerId = 0;

/// @brief 定时器标识。
using TimerId = std::uint64_t;

/// @brief 定时器回调。
using TimerCallback = std::function<void()>;

/// @brief 定时器管理器（基于 asio::steady_timer）。
///
/// 使用 asio::io_context 在独立线程运行，支持一次性与周期性定时器。
/// 定时器回调在 io 线程内执行，应尽快返回。
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
    // 调度一次异步等待（周期定时器到期后重新调度）。
    void Schedule(std::shared_ptr<asio::steady_timer> timer, TimerId id,
                  std::int64_t intervalMs, const TimerCallback& callback);

    // 添加定时器。
    TimerId AddTimerInternal(std::int64_t delayMs, std::int64_t intervalMs,
                             const TimerCallback& callback);

    asio::io_context io_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type> > work_;
    std::map<TimerId, std::shared_ptr<asio::steady_timer> > timers_;
    mutable std::mutex mutex_;
    std::thread thread_;
    TimerId nextId_;
    std::atomic<bool> running_;
};

} // namespace common
