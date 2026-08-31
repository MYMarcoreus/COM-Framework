#pragma once

#include <cstdint>

#include "Application/MyApplication.h"
#include "Module/ScopedInterfacePtr.h"
#include "Event/IEventDispatcher.h"

namespace servertemplate {

/// @brief ServerTemplate 服务器应用程序（ServerCore 最小复用骨架）。
///
/// 作为第一个业务服务器项目，复用 ServerCore 的模块 / 事件机制：
/// - RegisterModules：先调用基类默认装配（IConfig/ILogger/IMetrics），
///   再注册业务骨架模块（网络 / 事件 / 回显 / 通用 TCP 装配）。
///
/// 不含具体业务（规范：第一阶段不做业务认证 / 权限 / 数据库等）。
class CTemplateApplication : public sc::CMyApplication
{
public:
    explicit CTemplateApplication(std::uint16_t port);

    virtual ~CTemplateApplication();

protected:
    // 注册模块：接口模块（网络/事件/配置/日志/回显）+ 业务模块（日志/网络）。
    bool RegisterModules() override;

    // 初始化完成钩子：订阅网络生命周期事件。
    bool OnInitialize() override;

    // 启动完成钩子。
    bool OnStart() override;

    // 关闭钩子：取消订阅并释放引用。
    void OnShutdown() override;

private:
    std::uint16_t m_nPort;
    sc::ScopedInterfacePtr<sc::IEventDispatcher> m_pEventDispatcher;
    sc::SubscriptionId m_tEventStartId;
    sc::SubscriptionId m_tEventStopId;
};

} // namespace servertemplate
