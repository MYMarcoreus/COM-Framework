#include "Module/DemoTimerModule.h"

#include "Component/ScopedInterfacePtr.h"
#include "Log/Logger.h"

namespace demo {

/// @brief 创建定时器模块。
///
/// @param intervalMs 周期日志间隔（毫秒），小于 100 时按 100 处理。
CDemoTimerModule::CDemoTimerModule(std::int64_t intervalMs)
    : sc::CModule("timer"), m_nIntervalMs(intervalMs), m_tTimerId(common::kInvalidTimerId)
{
    if (m_nIntervalMs < 100)
    {
        m_nIntervalMs = 100;
    }
}

/// @brief 销毁定时器模块。
CDemoTimerModule::~CDemoTimerModule()
{
    Stop();
}

/// @brief 启动周期性定时器。
///
/// 每个周期输出一次运行状态日志，验证 Timer 模块与模块生命周期联动。
///
/// @return true 启动成功；false 启动失败。
bool CDemoTimerModule::Start()
{
    if (!m_timer.Start())
    {
        return false;
    }
    // 注册定时器回调时一并传入指向自身的强引用（Self 自持引用）：
    // 回调执行期间模块必然存活（即使正在关闭），彻底避免野指针；
    // 回调对象在定时器被取消/停止后销毁，自持引用随之释放。
    sc::ScopedInterfacePtr<sc::IModule> spSelf = Self();
    m_tTimerId = m_timer.AddPeriodicTimer(m_nIntervalMs,
        [spSelf]()
        {
            if (spSelf)
            {
                std::string strMessage = "Demo 服务器运行中 (module=";
                strMessage += spSelf->GetName();
                strMessage += ")";
                common::CLogger::Instance().Info(strMessage);
            }
        });
    return true;
}

/// @brief 停止定时器。
///
/// 先取消定时器（释放回调持有的自持引用，引用计数归零后模块可安全析构），
/// 再停止定时器线程。
void CDemoTimerModule::Stop()
{
    if (m_tTimerId != common::kInvalidTimerId)
    {
        m_timer.Cancel(m_tTimerId);
        m_tTimerId = common::kInvalidTimerId;
    }
    m_timer.Stop();
}

/// @brief 停止定时器并释放资源。
void CDemoTimerModule::Shutdown()
{
    Stop();
}

} // namespace demo
