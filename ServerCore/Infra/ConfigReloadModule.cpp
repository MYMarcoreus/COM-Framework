#include "Infra/ConfigReloadModule.h"

#include <string>

#include "Infra/GuardedTimer.h"
#include "Log/Logger.h"
#include "Module/ResolveContext.h"

namespace sc {

/// @brief 创建配置热加载模块。
///
/// @param nIntervalMs 重载检测周期（毫秒，<100 按 100 处理）。
CConfigReloadModule::CConfigReloadModule(std::int64_t nIntervalMs)
    : CModule("config-reload"),
      m_nIntervalMs(nIntervalMs), m_tTimerId(common::timer::kInvalidTimerId)
{
    // 依赖配置接口模块：拓扑排序保证其先初始化 / 启动。
    AddDependency(IID_IConfig());
    if (m_nIntervalMs < 100)
    {
        m_nIntervalMs = 100;
    }
}

/// @brief 销毁配置热加载模块。
CConfigReloadModule::~CConfigReloadModule()
{
    Stop();
}

/// @brief 从初始化上下文解析 IConfig 与可选的事件分发器。
///
/// @param ctx 初始化上下文（按类型自动绑定接口标识）。
///
/// @return true 配置模块存在；false 配置模块缺失。
bool CConfigReloadModule::Initialize(const CResolveContext& ctx)
{
    m_pConfig.Reset(ctx.Resolve<IConfig>());
    if (m_pConfig == nullptr)
    {
        return false;
    }
    // 事件分发器为可选依赖：缺失时不广播，仅执行重载。
    m_pEventDispatcher.Reset(ctx.Resolve<IEventDispatcher>());
    return true;
}

/// @brief 启动周期重载定时器。
///
/// @return true 启动成功；false 定时器启动失败。
bool CConfigReloadModule::Start()
{
    if (!m_timer.Start())
    {
        return false;
    }
    // 周期定时任务：用模板守卫函数统一处理弱引用生命周期，
    // 回调参数为具体类型强引用（无需转换）。
    m_tTimerId = sc::AddGuardedPeriodicTimer(&m_timer, m_nIntervalMs,
        WeakSelf<CConfigReloadModule>(),
        [](const sc::ScopedInterfacePtr<CConfigReloadModule>& sp)
        {
            sp->CheckReload();
        });
    return true;
}

/// @brief 停止定时器。
void CConfigReloadModule::Stop()
{
    if (m_tTimerId != common::timer::kInvalidTimerId)
    {
        m_timer.Cancel(m_tTimerId);
        m_tTimerId = common::timer::kInvalidTimerId;
    }
    m_timer.Stop();
}

/// @brief 停止定时器并释放引用。
void CConfigReloadModule::Shutdown()
{
    Stop();
    m_pConfig.Reset();
    m_pEventDispatcher.Reset();
}

/// @brief 周期重载检查。
///
/// 调用 IConfig::ReloadIfChanged()；发生变更时发布 "config.reloaded" 事件。
void CConfigReloadModule::CheckReload()
{
    if (m_pConfig == nullptr)
    {
        return;
    }
    if (m_pConfig->ReloadIfChanged())
    {
        if (m_pEventDispatcher != nullptr)
        {
            m_pEventDispatcher->Publish(sc::events::kConfigReloaded, nullptr, 0);
        }
        common::log::CLogger::Instance().Info("配置已热加载，发布 config.reloaded 事件");
    }
}

} // namespace sc
