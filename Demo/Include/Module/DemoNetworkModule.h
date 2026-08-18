#pragma once

#include <cstdint>

#include "Component/ComponentManager.h"
#include "Component/ScopedInterfacePtr.h"
#include "Event/IEventDispatcher.h"
#include "Module/Module.h"
#include "Network/INetwork.h"
#include "Network/INetworkHandler.h"

namespace demo {

// 前置声明，减少头文件依赖。
class DemoService;

/// @brief 网络模块。
///
/// 从组件管理器获取网络组件与协议处理服务，建立关联并启动 TCP 服务器。
/// 启动/停止时通过事件分发器发布事件，供其他模块解耦订阅。
/// 模块名 "network"，依赖组件注册与日志模块。
class DemoNetworkModule : public sc::Module
{
public:
    DemoNetworkModule(sc::ComponentManager& componentManager, DemoService* service,
                      std::uint16_t port);

    virtual ~DemoNetworkModule();

    // 从组件管理器获取网络接口并建立关联。
    bool Initialize() override;

    // 启动 TCP 服务器，并发布启动事件。
    bool Start() override;

    // 停止服务器，并发布停止事件。
    void Stop() override;

    // 停止服务器并释放引用。
    void Shutdown() override;

private:
    sc::ComponentManager& componentManager_;
    DemoService* service_; // 借用指针，由组件管理器持有
    std::uint16_t port_;
    sc::ScopedInterfacePtr<sc::INetwork> network_;
    sc::ScopedInterfacePtr<sc::INetworkHandler> handler_;
    sc::ScopedInterfacePtr<sc::IEventDispatcher> eventDispatcher_;
};

} // namespace demo
