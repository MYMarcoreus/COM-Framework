#include "Infra/ITimer.h"

#include <string>

namespace sc {

/// @brief 创建定时器模块。
CTimerModule::CTimerModule() : CModule("timer")
{
}

/// @brief 销毁定时器模块。
CTimerModule::~CTimerModule()
{
}

/// @brief 启动定时器线程。
bool CTimerModule::Start()
{
    return m_timer.Start();
}

/// @brief 添加一次性定时器。
common::TimerId CTimerModule::AddTimer(std::int64_t delayMs,
                                         const common::TimerCallback& callback)
{
    return m_timer.AddTimer(delayMs, callback);
}

/// @brief 添加周期性定时器。
common::TimerId CTimerModule::AddPeriodicTimer(std::int64_t intervalMs,
                                                 const common::TimerCallback& callback)
{
    return m_timer.AddPeriodicTimer(intervalMs, callback);
}

/// @brief 取消定时器。
bool CTimerModule::Cancel(common::TimerId id)
{
    return m_timer.Cancel(id);
}

/// @brief 停止定时器线程。
void CTimerModule::Stop()
{
    m_timer.Stop();
}

/// @brief 接口查询实现。
bool CTimerModule::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_ITimer()))
    {
        *ppv = static_cast<ITimer*>(this);
        return true;
    }
    return CModule::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
