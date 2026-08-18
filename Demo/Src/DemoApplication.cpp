#include "DemoApplication.h"

#include "Network/NetworkComponent.h"
#include "Service/DemoService.h"

namespace demo {

/// @brief 创建 Demo 服务器应用程序。
///
/// @param port 监听端口。
DemoApplication::DemoApplication(std::uint16_t port)
    : port_(port), service_(nullptr)
{
}

/// @brief 销毁 Demo 服务器应用程序。
DemoApplication::~DemoApplication()
{
}

/// @brief 注册组件。
///
/// 注册网络组件（INetwork）与协议处理服务（INetworkHandler）。
bool DemoApplication::RegisterComponents()
{
    // ① 注册网络组件
    sc::IUnknown* network = new sc::NetworkComponent();
    if (!componentManager_.RegisterComponent(sc::IID_INetwork(), network))
    {
        network->Release();
        return false;
    }
    network->Release(); // 管理器已持有引用

    // ② 注册协议处理服务
    DemoService* service = new DemoService();
    if (!componentManager_.RegisterComponent(sc::IID_INetworkHandler(), service))
    {
        service->Release();
        return false;
    }
    service_ = service;
    service->Release(); // 管理器已持有引用
    return true;
}

/// @brief 初始化完成钩子。
///
/// 通过组件管理器获取网络接口与处理接口，建立关联。
bool DemoApplication::OnInitialize()
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

    // ③ 告诉服务网络引用，用于发送响应
    if (service_ != nullptr)
    {
        service_->SetNetwork(network_.Get());
    }
    return true;
}

/// @brief 启动钩子。
///
/// 启动 TCP 服务器并监听指定端口。
bool DemoApplication::OnStart()
{
    if (network_ == nullptr || handler_ == nullptr)
    {
        return false;
    }
    return network_->StartTcpServer(port_, handler_.Get());
}

/// @brief 关闭钩子。
///
/// 停止网络服务器并释放组件引用。
void DemoApplication::OnShutdown()
{
    if (network_ != nullptr)
    {
        network_->Stop();
    }
    network_.Reset();
    handler_.Reset();
    service_ = nullptr;
}

} // namespace demo
