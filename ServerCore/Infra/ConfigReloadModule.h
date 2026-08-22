#pragma once

#include <cstdint>

#include "Module/ScopedInterfacePtr.h"
#include "Event/IEventDispatcher.h"
#include "Infra/IConfig.h"
#include "Module/Module.h"
#include "Module/ModuleManager.h"
#include "Timer/TimerManager.h"

namespace sc {

/// @brief 配置热加载模块。
///
/// 周期性调用 IConfig::ReloadIfChanged()，检测到配置变更时通过事件分发器
/// 发布 "config.reloaded" 事件（负载为空），供订阅方感知并重新读取配置。
/// 依赖 IConfig 模块；IEventDispatcher 为可选依赖（缺失时不广播，仅重载）。
/// 模块名 "config-reload"。
class CConfigReloadModule : public CModule
{
public:
    // 创建配置热加载模块。
    // @param nIntervalMs 重载检测周期（毫秒，<100 按 100 处理，默认 5000）。
    CConfigReloadModule(std::int64_t nIntervalMs = 5000);

    virtual ~CConfigReloadModule();

    // 从初始化上下文解析 IConfig / IEventDispatcher。
    bool Initialize(const CResolveContext& ctx) override;

    // 启动周期重载定时器。
    bool Start() override;

    // 停止定时器。
    void Stop() override;

    // 停止定时器并释放引用。
    void Shutdown() override;

private:
    // 周期重载检查：发生变更时发布 config.reloaded 事件。
    void CheckReload();

    std::int64_t m_nIntervalMs;
    common::CTimerManager m_timer;
    common::TimerId m_tTimerId;
    ScopedInterfacePtr<IConfig> m_pConfig;
    ScopedInterfacePtr<IEventDispatcher> m_pEventDispatcher;
};

} // namespace sc
