#pragma once

#include <cstdint>

#include "Application/MyApplication.h"
#include "Module/ScopedInterfacePtr.h"
#include "Config/Config.h"
#include "Event/IEventDispatcher.h"

namespace demo {

/// @brief Demo 服务器应用程序。
///
/// 验证 ServerCore：注册网络模块与协议处理服务，通过 CModuleManager
/// 统一管理日志 / 定时器 / 网络模块的生命周期，并通过事件分发器
/// 订阅网络模块发布的事件（模块间解耦通信）。
class CDemoApplication : public sc::CMyApplication
{
public:
    explicit CDemoApplication(std::uint16_t port);

    virtual ~CDemoApplication();

protected:
    // 注册模块：默认装配 + 接口模块（网络/事件/服务）+ 业务模块（日志/定时器/网络）。
    bool RegisterModules() override;

    // 初始化完成钩子：订阅网络生命周期事件。
    bool OnInitialize() override;

    // 启动完成钩子。
    bool OnStart() override;

    // 关闭钩子：取消订阅并释放引用。
    void OnShutdown() override;

private:
    std::uint16_t m_nPort;
    common::config::CConfig m_config;
    sc::ScopedInterfacePtr<sc::IEventDispatcher> m_pEventDispatcher;
    sc::SubscriptionId m_tEventStartId;
    sc::SubscriptionId m_tEventStopId;
    sc::SubscriptionId m_tDemoEventId;
};

} // namespace demo
