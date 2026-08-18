#pragma once

#include <cstdint>

#include "Application/MyApplication.h"
#include "Component/ScopedInterfacePtr.h"
#include "Event/IEventDispatcher.h"

namespace servera {

// 前置声明，减少头文件依赖。
class EchoService;

/// @brief ServerA 服务器应用程序（ServerCore 复用骨架）。
///
/// 作为第一个业务服务器项目，复用 ServerCore 的组件 / 模块 / 事件机制：
/// - RegisterComponents：注册网络组件、事件分发器、回显服务
/// - RegisterModules：注册日志模块与网络模块
///
/// 不含具体业务（规范：第一阶段不做业务认证 / 权限 / 数据库等）。
class ServerApplication : public sc::MyApplication
{
public:
    explicit ServerApplication(std::uint16_t port);

    virtual ~ServerApplication();

protected:
    // 注册组件：网络组件、事件分发器与回显服务。
    bool RegisterComponents() override;

    // 注册模块：日志 / 网络。
    bool RegisterModules() override;

    // 初始化完成钩子：订阅网络生命周期事件。
    bool OnInitialize() override;

    // 启动完成钩子。
    bool OnStart() override;

    // 关闭钩子：取消订阅并释放引用。
    void OnShutdown() override;

private:
    std::uint16_t port_;
    EchoService* service_; // 借用指针，由组件管理器持有
    sc::ScopedInterfacePtr<sc::IEventDispatcher> eventDispatcher_;
    sc::SubscriptionId eventStartId_;
    sc::SubscriptionId eventStopId_;
};

} // namespace servera
