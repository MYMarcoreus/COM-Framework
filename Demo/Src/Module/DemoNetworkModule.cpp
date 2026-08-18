#include "Module/DemoNetworkModule.h"

#include <string>

#include "Log/Logger.h"
#include "Service/DemoService.h"

namespace demo {

/// @brief 创建网络模块。
///
/// @param componentManager 组件管理器，用于获取网络与协议处理组件。
/// @param service          协议处理服务（借用指针，由组件管理器持有）。
/// @param port             监听端口。
DemoNetworkModule::DemoNetworkModule(sc::ComponentManager& componentManager,
                                     DemoService* service, std::uint16_t port)
    : sc::Module("network"), componentManager_(componentManager),
      service_(service), port_(port)
{
}

/// @brief 销毁网络模块。
DemoNetworkModule::~DemoNetworkModule()
{
}

/// @brief 从组件管理器获取网络接口并建立关联。
///
/// 获取 INetwork 与 INetworkHandler 接口，并将网络引用注入协议服务，
/// 使服务能够发送响应。
///
/// @return true 关联成功；false 组件缺失。
bool DemoNetworkModule::Initialize()
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

    // ② 获取协议处理接口
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

    // ③ 注入网络引用到协议服务，用于发送响应
    if (service_ != nullptr)
    {
        service_->SetNetwork(network_.Get());
    }
    common::Logger::Instance().Info("Demo 服务器初始化中，监听端口 " + std::to_string(port_));
    return true;
}

/// @brief 启动 TCP 服务器。
///
/// @return true 启动成功；false 启动失败（如端口被占用）。
bool DemoNetworkModule::Start()
{
    if (network_ == nullptr || handler_ == nullptr)
    {
        return false;
    }
    if (!network_->StartTcpServer(port_, handler_.Get()))
    {
        return false;
    }
    common::Logger::Instance().Info(
        "Demo 服务器已启动，监听端口 " + std::to_string(port_) + "，按 Ctrl+C 退出");
    return true;
}

/// @brief 停止服务器。
void DemoNetworkModule::Stop()
{
    if (network_ != nullptr)
    {
        network_->Stop();
    }
}

/// @brief 停止服务器并释放引用。
void DemoNetworkModule::Shutdown()
{
    Stop();
    network_.Reset();
    handler_.Reset();
    service_ = nullptr;
}

} // namespace demo
