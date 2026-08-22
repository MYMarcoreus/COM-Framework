#include "Module/DemoTimerModule.h"

#include "Infra/GuardedTimer.h"
#include "Log/Logger.h"
#include "Module/ResolveContext.h"
#include "Module/ScopedInterfacePtr.h"

namespace demo {

/// @brief 创建定时器模块。
///
/// @param intervalMs 周期日志间隔（毫秒），小于 100 时按 100 处理。
CDemoTimerModule::CDemoTimerModule(std::int64_t intervalMs)
    : sc::CModule("timer"), m_nIntervalMs(intervalMs), m_tTimerId(common::kInvalidTimerId)
{
    // 依赖 ITimer 接口模块：生命周期拓扑排序保证其先初始化 / 启动。
    AddDependency(sc::IID_ITimer());
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

/// @brief 从初始化上下文解析 ITimer 接口。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true 定时器接口就绪；false 缺失。
bool CDemoTimerModule::Initialize(const sc::CResolveContext& ctx)
{
    m_pTimer.Reset(ctx.Resolve<sc::ITimer>());
    return m_pTimer != nullptr;
}

/// @brief 启动周期性定时器。
///
/// 每个周期输出一次运行状态日志，验证 Timer 接口与模块生命周期联动。
///
/// @return true 启动成功；false 定时器接口缺失。
bool CDemoTimerModule::Start()
{
    if (m_pTimer == nullptr)
    {
        return false;
    }
    // 周期定时任务：用模板守卫函数统一处理弱引用生命周期，
    // 回调参数为已升级的强引用（类型随弱引用，无需转换）。
    m_tTimerId = sc::AddGuardedPeriodicTimer(m_pTimer.Get(), m_nIntervalMs,
        WeakSelf(),
        [](const sc::ScopedInterfacePtr<sc::IModule>& sp)
        {
            std::string strMessage = "Demo 服务器运行中 (module=";
            strMessage += sp->GetName();
            strMessage += ")";
            common::CLogger::Instance().Info(strMessage);
        });
    return true;
}

/// @brief 停止定时器。
///
/// 只取消本模块注册的定时器（释放回调持有的自持引用）；
/// 共享的 ITimer 线程由 TimerModule 生命周期统一管理。
void CDemoTimerModule::Stop()
{
    if (m_tTimerId != common::kInvalidTimerId)
    {
        if (m_pTimer != nullptr)
        {
            m_pTimer->Cancel(m_tTimerId);
        }
        m_tTimerId = common::kInvalidTimerId;
    }
}

/// @brief 停止定时器并释放接口引用。
void CDemoTimerModule::Shutdown()
{
    Stop();
    m_pTimer.Reset();
}

} // namespace demo
