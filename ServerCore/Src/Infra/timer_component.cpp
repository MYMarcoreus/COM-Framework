#include "Infra/i_timer.h"

#include <string>

namespace sc {

/// @brief 创建定时器组件。
CTimerComponent::CTimerComponent()
{
}

/// @brief 销毁定时器组件。
CTimerComponent::~CTimerComponent()
{
}

/// @brief 启动定时器线程。
bool CTimerComponent::Start()
{
    return m_timer.Start();
}

/// @brief 添加一次性定时器。
common::TimerId CTimerComponent::AddTimer(std::int64_t delayMs,
                                         const common::TimerCallback& callback)
{
    return m_timer.AddTimer(delayMs, callback);
}

/// @brief 添加周期性定时器。
common::TimerId CTimerComponent::AddPeriodicTimer(std::int64_t intervalMs,
                                                 const common::TimerCallback& callback)
{
    return m_timer.AddPeriodicTimer(intervalMs, callback);
}

/// @brief 取消定时器。
bool CTimerComponent::Cancel(common::TimerId id)
{
    return m_timer.Cancel(id);
}

/// @brief 停止定时器线程。
void CTimerComponent::Stop()
{
    m_timer.Stop();
}

/// @brief 接口查询实现。
bool CTimerComponent::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_ITimer()))
    {
        *ppv = static_cast<ITimer*>(this);
        return true;
    }
    return CComponent::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
