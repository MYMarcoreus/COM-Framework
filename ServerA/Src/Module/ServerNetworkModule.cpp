#include "Module/ServerNetworkModule.h"

#include <string>

#include "Log/Logger.h"
#include "Service/EchoService.h"

namespace servera {

/// @brief 创建网络模块。
///
/// @param componentManager 组件管理器，用于获取网络与回显服务组件。
/// @param service          回显服务（借用指针，由组件管理器持有）。
/// @param port             监听端口。
ServerNetworkModule::ServerNetworkModule(sc::ComponentManager& componentManager,
                                         EchoService* service, std::uint16_t port)
    : sc::Module("network"), componentManager_(componentManager),
      service_(service), port_(port)
{
}

/// @brief 销毁网络模块。
ServerNetworkModule::~ServerNetworkModule()
{
}

/// @brief 从组件管理器获取网络接口并建立关联。
///
/// @return true 关联成功；false 组件缺失。
bool ServerNetworkModule::Initialize()
{
    // ① 获取网络接口
    sc::IUnknown* networkObject = componentManager_.GetComponent(sc::IID_INetwork());
    if (networkObject == nullptr)
    {
        return false;
    }
    void* raw = nullptr;
    if (!networkObject->QueryInterface(sc::IID_INetwork(), &raw))
    {
        return false;
    }
    network_.Reset(static_cast<sc::INetwork*>(raw));

    // ② 获取回显服务接口
    sc::IUnknown* serviceObject = componentManager_.GetComponent(sc::IID_INetworkHandler());
    if (serviceObject == nullptr)
    {
        return false;
    }
    if (!serviceObject->QueryInterface(sc::IID_INetworkHandler(), &raw))
    {
        return false;
    }
    handler_.Reset(static_cast<sc::INetworkHandler*>(raw));

    // ③ 获取事件分发器
    sc::IUnknown* eventObject = componentManager_.GetComponent(sc::IID_EventDispatcher());
    if (eventObject != nullptr)
    {
        if (eventObject->QueryInterface(sc::IID_EventDispatcher(), &raw))
        {
            eventDispatcher_.Reset(static_cast<sc::IEventDispatcher*>(raw));
        }
    }

    // ④ 注入网络引用到回显服务
    if (service_ != nullptr)
    {
        service_->SetNetwork(network_.Get());
    }
    common::Logger::Instance().Info("ServerA 初始化中，监听端口 " + std::to_string(port_));
    return true;
}

/// @brief 启动 TCP 服务器。
///
/// 启动成功后发布 "network.started" 事件（负载为监听端口）。
///
/// @return true 启动成功；false 启动失败。
bool ServerNetworkModule::Start()
{
    if (network_ == nullptr || handler_ == nullptr)
    {
        return false;
    }
    if (!network_->StartTcpServer(port_, handler_.Get()))
    {
        return false;
    }
    if (eventDispatcher_ != nullptr)
    {
        eventDispatcher_->Publish("network.started", &port_, sizeof(port_));
    }
    common::Logger::Instance().Info(
        "ServerA 已启动，监听端口 " + std::to_string(port_) + "，按 Ctrl+C 退出");
    return true;
}

/// @brief 停止服务器。
void ServerNetworkModule::Stop()
{
    if (eventDispatcher_ != nullptr)
    {
        eventDispatcher_->Publish("network.stopped", nullptr, 0);
    }
    if (network_ != nullptr)
    {
        network_->Stop();
    }
}

/// @brief 停止服务器并释放引用。
void ServerNetworkModule::Shutdown()
{
    Stop();
    network_.Reset();
    handler_.Reset();
    eventDispatcher_.Reset();
    service_ = nullptr;
}

} // namespace servera
