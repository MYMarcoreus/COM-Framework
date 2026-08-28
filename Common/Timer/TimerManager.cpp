#include "Timer/TimerManager.h"

namespace common {
namespace timer {

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
TimerId CTimerManager::AddTimer(std::int64_t nDelayMs, const TimerCallback& fnCallback)
{
    return AddTimerInternal(nDelayMs, 0, fnCallback);
}

/// @brief 添加周期性定时器。
///
/// @param intervalMs 周期毫秒数。
/// @param callback 每次到期回调。
///
/// @return 定时器标识；失败返回 kInvalidTimerId。
TimerId CTimerManager::AddPeriodicTimer(std::int64_t nIntervalMs, const TimerCallback& fnCallback)
{
    return AddTimerInternal(0, nIntervalMs, fnCallback);
}

/// @brief 取消定时器。
bool CTimerManager::Cancel(TimerId nId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<TimerId, std::shared_ptr<asio::steady_timer> >::iterator it = m_mapTimers.find(nId);
    if (it == m_mapTimers.end())
    {
        return false;
    }
    static_cast<void>(it->second->cancel());
    m_mapTimers.erase(it); // 到期处理函数稍后以 operation_aborted 触发（幂等）
    return true;
}

/// @brief 注册命名周期定时器。
///
/// 通过名称管理定时器，便于后续按名称取消。
///
/// @param strName 定时器名称（进程内唯一）。
/// @param nIntervalMs 周期毫秒数。
/// @param fnCallback 每次到期回调。
///
/// @return 定时器标识；失败返回 kInvalidTimerId。
TimerId CTimerManager::AddNamedTimer(const std::string& strName, std::int64_t nIntervalMs,
                                    const TimerCallback& fnCallback)
{
    TimerId nId = AddTimerInternal(0, nIntervalMs, fnCallback);
    if (nId != kInvalidTimerId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mapNamedTimers[strName] = nId;
    }
    return nId;
}

/// @brief 按名称取消定时器。
///
/// @param strName 定时器名称。
///
/// @return true 取消成功；false 名称不存在。
bool CTimerManager::CancelNamedTimer(const std::string& strName)
{
    TimerId nId = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<std::string, TimerId>::iterator it = m_mapNamedTimers.find(strName);
        if (it == m_mapNamedTimers.end())
        {
            return false;
        }
        nId = it->second;
        m_mapNamedTimers.erase(it);
    }
    return Cancel(nId);
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
TimerId CTimerManager::AddTimerInternal(std::int64_t nDelayMs, std::int64_t nIntervalMs,
                                       const TimerCallback& fnCallback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_bRunning.load())
    {
        return kInvalidTimerId;
    }
    TimerId nId = m_nNextId++;
    std::shared_ptr<asio::steady_timer> pTimer(new asio::steady_timer(m_io));
    m_mapTimers[nId] = pTimer;
    std::int64_t nDelay = (nIntervalMs > 0) ? nIntervalMs : nDelayMs;
    pTimer->expires_after(std::chrono::milliseconds(nDelay));
    Schedule(pTimer, nId, nIntervalMs, fnCallback);
    return nId;
}

/// @brief 调度一次异步等待。
void CTimerManager::Schedule(std::shared_ptr<asio::steady_timer> pTimer, TimerId nId,
                            std::int64_t nIntervalMs, const TimerCallback& fnCallback)
{
    pTimer->async_wait(
        [this, pTimer, nId, nIntervalMs, fnCallback](const asio::error_code& ec)
        {
            // ① 取消或错误：清理定时器
            if (ec)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_mapTimers.erase(nId);
                return;
            }
            // ② 触发回调
            if (fnCallback)
            {
                fnCallback();
            }
            // ③ 周期性定时器重新调度（若仍被管理）
            if (nIntervalMs > 0)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                std::map<TimerId, std::shared_ptr<asio::steady_timer> >::iterator it =
                    m_mapTimers.find(nId);
                if (it == m_mapTimers.end() || it->second != pTimer)
                {
                    return; // 已被取消
                }
                pTimer->expires_after(std::chrono::milliseconds(nIntervalMs));
                Schedule(pTimer, nId, nIntervalMs, fnCallback);
            }
            else
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_mapTimers.erase(nId);
            }
        });
}

} // namespace timer
} // namespace common
