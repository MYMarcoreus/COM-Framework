#include "Infra/TimerModule.h"

#include "Module/InterfaceMap.h"
#include <string>

namespace sc {

// 接口映射表：暴露本类实现的接口（查表驱动 QueryInterface）。
SC_DEFINE_INTERFACE_MAP(CTimerModule, CModule, ITimer)

/// @brief 创建定时器模块。
CTimerModule::CTimerModule() : CModule("timer")
{
}

/// @brief 销毁定时器模块。
CTimerModule::~CTimerModule()
{
}

/// @brief 初始化模块（无配置依赖，直接成功）。
bool CTimerModule::Initialize(const CResolveContext& /*ctx*/)
{
    return true;
}

/// @brief 启动定时器线程。
bool CTimerModule::Start()
{
    return m_timer.Start();
}

/// @brief 添加一次性定时器。
common::timer::TimerId CTimerModule::AddTimer(std::int64_t delayMs,
                                                const common::timer::TimerCallback& callback)
{
    return m_timer.AddTimer(delayMs, callback);
}

/// @brief 添加周期性定时器。
common::timer::TimerId CTimerModule::AddPeriodicTimer(std::int64_t intervalMs,
                                                        const common::timer::TimerCallback& callback)
{
    return m_timer.AddPeriodicTimer(intervalMs, callback);
}

/// @brief 取消定时器。
bool CTimerModule::Cancel(common::timer::TimerId id)
{
    return m_timer.Cancel(id);
}

/// @brief 停止定时器线程。
void CTimerModule::Stop()
{
    m_timer.Stop();
}

/// @brief 关闭模块（定时器资源由 Stop 释放，无需额外处理）。
void CTimerModule::Shutdown()
{
}

} // namespace sc
