#include "Service/DemoService.h"

#include "Log/Logger.h"

namespace demo {

/// @brief 创建 Demo 协议处理服务。
DemoService::DemoService()
{
}

/// @brief 销毁 Demo 协议处理服务。
DemoService::~DemoService()
{
}

/// @brief 设置网络组件引用。
///
/// @param network 网络组件接口（以引用计数方式持有）。
void DemoService::SetNetwork(sc::INetwork* network)
{
    network_.Reset(network);
}

/// @brief 新连接建立回调。
void DemoService::OnAccept(sc::ConnectionId id, const std::string& peer)
{
    Log("连接建立: id=" + std::to_string(id) + " peer=" + peer);
}

/// @brief 收到数据回调。
///
/// 数据追加到该连接的待解析缓冲，尝试解析完整报文。
void DemoService::OnData(sc::ConnectionId id, const char* data, size_t len)
{
    std::string& pending = pendingBuffers_[id];
    pending.append(data, len);

    size_t consumed = 0;
    Packet packet;
    while (true)
    {
        // ① 尝试解析一个完整报文
        ParseResult result = DemoProtocol::ParsePacket(pending, &consumed, &packet);
        if (result == ParseResult::kNeedMore)
        {
            break; // 等待更多数据
        }
        if (result == ParseResult::kInvalid)
        {
            // ② 协议非法，丢弃缓冲并关闭连接
            pending.clear();
            Log("协议错误，关闭连接: id=" + std::to_string(id));
            if (network_ != nullptr)
            {
                network_->Close(id);
            }
            return;
        }
        // ③ 移除已消费的字节并处理报文
        pending.erase(0, consumed);
        HandlePacket(id, packet);
    }
}

/// @brief 连接关闭回调。
void DemoService::OnClose(sc::ConnectionId id)
{
    Log("连接关闭: id=" + std::to_string(id));
    pendingBuffers_.erase(id);
}

/// @brief 处理一个完整报文。
///
/// 支持 PING（返回 PONG）与 ECHO（返回相同负载）。
void DemoService::HandlePacket(sc::ConnectionId id, const Packet& packet)
{
    if (packet.command == kCmdPing)
    {
        std::string response = DemoProtocol::BuildPong();
        Log("收到 PING，返回 PONG: id=" + std::to_string(id));
        if (network_ != nullptr)
        {
            network_->Send(id, response.data(), response.size());
        }
        return;
    }
    if (packet.command == kCmdEcho)
    {
        std::string response = DemoProtocol::BuildPacket(kCmdEcho, packet.payload);
        Log("收到 ECHO，负载长度=" + std::to_string(packet.payload.size()) + " id=" + std::to_string(id));
        if (network_ != nullptr)
        {
            network_->Send(id, response.data(), response.size());
        }
        return;
    }
    // 未知命令：忽略
    Log("未知命令: " + std::to_string(packet.command) + " id=" + std::to_string(id));
}

/// @brief 接口查询实现。
bool DemoService::QueryInterfaceImpl(const sc::InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(sc::IID_INetworkHandler()))
    {
        *ppv = static_cast<sc::INetworkHandler*>(this);
        return true;
    }
    return sc::Component::QueryInterfaceImpl(iid, ppv);
}

/// @brief 记录日志。
void DemoService::Log(const std::string& message)
{
    common::Logger::Instance().Info("[DemoService] " + message);
}

} // namespace demo
