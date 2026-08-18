#include "Timer/timer_manager.h"

namespace common {

/// @brief 创建定时器管理器。
CTimerManager::CTimerManager()
    : m_nNextId(1), m_bRunning(false)
{
}

/// @brief 销毁定时器管理器。
CTimerManager::~CTimerManager()
{
    Stop();
}

/// @brief 启动定时器线程。
bool CTimerManager::Start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_bRunning.load())
    {
        return false;
    }
    m_io.restart();
    m_pWork.reset(new asio::executor_work_guard<asio::io_context::executor_type>(
        asio::make_work_guard(m_io)));
    m_bRunning.store(true);
    m_thread = std::thread([this]() { m_io.run(); });
    return true;
}

/// @brief 添加一次性定时器。
///
/// @param delayMs 延迟毫秒数。
/// @param callback 到期回调。
///
/// @return 定时器标识；失败返回 kInvalidTimerId。
TimerId CTimerManager::AddTimer(std::int64_t delayMs, const TimerCallback& callback)
{
    return AddTimerInternal(delayMs, 0, callback);
}

/// @brief 添加周期性定时器。
///
/// @param intervalMs 周期毫秒数。
/// @param callback 每次到期回调。
///
/// @return 定时器标识；失败返回 kInvalidTimerId。
TimerId CTimerManager::AddPeriodicTimer(std::int64_t intervalMs, const TimerCallback& callback)
{
    return AddTimerInternal(0, intervalMs, callback);
}

/// @brief 取消定时器。
bool CTimerManager::Cancel(TimerId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<TimerId, std::shared_ptr<asio::steady_timer> >::iterator it = m_mapTimers.find(id);
    if (it == m_mapTimers.end())
    {
        return false;
    }
    static_cast<void>(it->second->cancel());
    m_mapTimers.erase(it); // 到期处理函数稍后以 operation_aborted 触发（幂等）
    return true;
}

/// @brief 停止定时器线程。
void CTimerManager::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_bRunning.load())
        {
            return;
        }
        m_bRunning.store(false);
        for (std::map<TimerId, std::shared_ptr<asio::steady_timer> >::iterator it = m_mapTimers.begin();
             it != m_mapTimers.end(); ++it)
        {
            static_cast<void>(it->second->cancel());
        }
        m_mapTimers.clear();
        m_pWork.reset();
    }
    m_io.stop();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

/// @brief 是否正在运行。
bool CTimerManager::IsRunning() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bRunning.load();
}

/// @brief 添加定时器。
///
/// @param delayMs 一次性延迟（intervalMs 为 0 时生效）。
/// @param intervalMs 周期（大于 0 时表示周期性定时器）。
/// @param callback 回调。
TimerId CTimerManager::AddTimerInternal(std::int64_t delayMs, std::int64_t intervalMs,
                                       const TimerCallback& callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_bRunning.load())
    {
        return kInvalidTimerId;
    }
    TimerId id = m_nNextId++;
    std::shared_ptr<asio::steady_timer> timer(new asio::steady_timer(m_io));
    m_mapTimers[id] = timer;
    std::int64_t delay = (intervalMs > 0) ? intervalMs : delayMs;
    timer->expires_after(std::chrono::milliseconds(delay));
    Schedule(timer, id, intervalMs, callback);
    return id;
}

/// @brief 调度一次异步等待。
void CTimerManager::Schedule(std::shared_ptr<asio::steady_timer> timer, TimerId id,
                            std::int64_t intervalMs, const TimerCallback& callback)
{
    timer->async_wait(
        [this, timer, id, intervalMs, callback](const asio::error_code& ec)
        {
            // ① 取消或错误：清理定时器
            if (ec)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_mapTimers.erase(id);
                return;
            }
            // ② 触发回调
            if (callback)
            {
                callback();
            }
            // ③ 周期性定时器重新调度（若仍被管理）
            if (intervalMs > 0)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                std::map<TimerId, std::shared_ptr<asio::steady_timer> >::iterator it =
                    m_mapTimers.find(id);
                if (it == m_mapTimers.end() || it->second != timer)
                {
                    return; // 已被取消
                }
                timer->expires_after(std::chrono::milliseconds(intervalMs));
                Schedule(timer, id, intervalMs, callback);
            }
            else
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_mapTimers.erase(id);
            }
        });
}

} // namespace common
