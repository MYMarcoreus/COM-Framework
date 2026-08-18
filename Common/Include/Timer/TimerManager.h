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
class CTimerManager
{
public:
    CTimerManager();

    ~CTimerManager();

    // 启动定时器线程。
    bool Start();

    // 添加一次性定时器（nDelayMs 毫秒后触发一次）。
    TimerId AddTimer(std::int64_t nDelayMs, const TimerCallback& fnCallback);

    // 添加周期性定时器（每 nIntervalMs 毫秒触发一次）。
    TimerId AddPeriodicTimer(std::int64_t nIntervalMs, const TimerCallback& fnCallback);

    // 取消定时器。
    bool Cancel(TimerId nId);

    // 停止定时器线程。
    void Stop();

    // 是否正在运行。
    bool IsRunning() const;

private:
    // 调度一次异步等待（周期定时器到期后重新调度）。
    void Schedule(std::shared_ptr<asio::steady_timer> pTimer, TimerId nId,
                  std::int64_t nIntervalMs, const TimerCallback& fnCallback);

    // 添加定时器。
    TimerId AddTimerInternal(std::int64_t nDelayMs, std::int64_t nIntervalMs,
                             const TimerCallback& fnCallback);

    asio::io_context m_io;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type> > m_pWork;
    std::map<TimerId, std::shared_ptr<asio::steady_timer> > m_mapTimers;
    mutable std::mutex m_mutex;
    std::thread m_thread;
    TimerId m_nNextId;
    std::atomic<bool> m_bRunning;
};

} // namespace common
