#pragma once

#include <cstdint>

#include "Component/component_manager.h"
#include "Component/scoped_interface_ptr.h"
#include "Event/i_event_dispatcher.h"
#include "Module/module.h"
#include "Network/i_network.h"
#include "Network/i_network_handler.h"

namespace servera {

// 前置声明，减少头文件依赖。
class CEchoService;

/// @brief 网络模块。
///
/// 从组件管理器获取网络组件与回显服务，建立关联并启动 TCP 服务器。
/// 启动/停止时发布事件，供其他模块解耦订阅。
/// 模块名 "network"。
class CServerNetworkModule : public sc::CModule
{
public:
    CServerNetworkModule(sc::CComponentManager& componentManager, CEchoService* service,
                        std::uint16_t port);

    virtual ~CServerNetworkModule();

    // 从组件管理器获取网络接口并建立关联。
    bool Initialize() override;

    // 启动 TCP 服务器，并发布启动事件。
    bool Start() override;

    // 停止服务器，并发布停止事件。
    void Stop() override;

    // 停止服务器并释放引用。
    void Shutdown() override;

private:
    sc::CComponentManager& componentManager_;
    CEchoService* service_; // 借用指针，由组件管理器持有
    std::uint16_t port_;
    sc::ScopedInterfacePtr<sc::INetwork> network_;
    sc::ScopedInterfacePtr<sc::INetworkHandler> handler_;
    sc::ScopedInterfacePtr<sc::IEventDispatcher> eventDispatcher_;
};

} // namespace servera
