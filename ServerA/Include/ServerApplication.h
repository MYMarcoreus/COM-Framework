#pragma once

#include <cstdint>

#include "Application/MyApplication.h"
#include "Component/ScopedInterfacePtr.h"
#include "Event/IEventDispatcher.h"

namespace servera {

// 前置声明，减少头文件依赖。
class CEchoService;

/// @brief ServerA 服务器应用程序（ServerCore 复用骨架）。
///
/// 作为第一个业务服务器项目，复用 ServerCore 的模块 / 事件机制：
/// - RegisterModules：接口模块（网络/事件/配置/日志/回显）+ 业务模块（日志/网络）
///
/// 不含具体业务（规范：第一阶段不做业务认证 / 权限 / 数据库等）。
class CServerApplication : public sc::CMyApplication
{
public:
    explicit CServerApplication(std::uint16_t port);

    virtual ~CServerApplication();

protected:
    // 注册模块：默认装配 + 接口模块（网络/事件/配置/日志/回显）+ 业务模块（日志/网络）。
    bool RegisterModules() override;

    // 初始化完成钩子：订阅网络生命周期事件。
    bool OnInitialize() override;

    // 启动完成钩子。
    bool OnStart() override;

    // 关闭钩子：取消订阅并释放引用。
    void OnShutdown() override;

private:
    std::uint16_t m_nPort;
    CEchoService* m_pService; // 借用指针，由模块管理器持有
    sc::ScopedInterfacePtr<sc::IEventDispatcher> m_pEventDispatcher;
    sc::SubscriptionId m_tEventStartId;
    sc::SubscriptionId m_tEventStopId;
};

} // namespace servera
