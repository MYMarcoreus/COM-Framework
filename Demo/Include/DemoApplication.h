#pragma once

#include <cstdint>

#include "Application/MyApplication.h"
#include "Component/ScopedInterfacePtr.h"
#include "Config/Config.h"
#include "Event/IEventDispatcher.h"

namespace demo {

// 前置声明，减少头文件依赖。
class CDemoService;

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
    // 注册模块：网络模块、协议处理服务与事件分发器。
    bool RegisterComponents() override;

    // 注册模块：日志 / 定时器 / 网络。
    bool RegisterModules() override;

    // 初始化完成钩子：订阅网络生命周期事件。
    bool OnInitialize() override;

    // 启动完成钩子。
    bool OnStart() override;

    // 关闭钩子：取消订阅并释放引用。
    void OnShutdown() override;

private:
    std::uint16_t m_nPort;
    common::CConfig m_config;
    CDemoService* m_pService; // 借用指针，由模块管理器持有
    sc::ScopedInterfacePtr<sc::IEventDispatcher> m_pEventDispatcher;
    sc::SubscriptionId m_tEventStartId;
    sc::SubscriptionId m_tEventStopId;
};

} // namespace demo
