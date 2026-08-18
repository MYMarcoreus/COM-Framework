#include "Service/echo_service.h"

#include <string>

#include "Log/logger.h"

namespace servera {

/// @brief 创建回显服务。
CEchoService::CEchoService()
{
}

/// @brief 销毁回显服务。
CEchoService::~CEchoService()
{
}

/// @brief 设置网络组件引用。
///
/// @param network 网络组件接口（以引用计数方式持有）。
void CEchoService::SetNetwork(sc::INetwork* network)
{
    network_.Reset(network);
}

/// @brief 新连接建立回调。
void CEchoService::OnAccept(sc::ConnectionId id, const std::string& peer)
{
    Log("连接建立: id=" + std::to_string(id) + " peer=" + peer);
}

/// @brief 收到数据回调。
///
/// 原样返回收到的数据，验证 ServerCore 网络收发链路。
void CEchoService::OnData(sc::ConnectionId id, const char* data, size_t len)
{
    Log("收到数据: id=" + std::to_string(id) + " len=" + std::to_string(len));
    if (network_ != nullptr && data != nullptr && len > 0)
    {
        network_->Send(id, data, len);
    }
}

/// @brief 连接关闭回调。
void CEchoService::OnClose(sc::ConnectionId id)
{
    Log("连接关闭: id=" + std::to_string(id));
}

/// @brief 接口查询实现。
bool CEchoService::QueryInterfaceImpl(const sc::InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(sc::IID_INetworkHandler()))
    {
        *ppv = static_cast<sc::INetworkHandler*>(this);
        return true;
    }
    return sc::CComponent::QueryInterfaceImpl(iid, ppv);
}

/// @brief 记录日志。
void CEchoService::Log(const std::string& message)
{
    common::CLogger::Instance().Info("[CEchoService] " + message);
}

} // namespace servera
