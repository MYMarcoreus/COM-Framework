#include "Service/DemoService.h"

#include "Log/Logger.h"

namespace demo {

/// @brief 创建 Demo 协议处理服务。
CDemoService::CDemoService()
{
}

/// @brief 销毁 Demo 协议处理服务。
CDemoService::~CDemoService()
{
}

/// @brief 设置网络组件引用。
///
/// @param network 网络组件接口（以引用计数方式持有）。
void CDemoService::SetNetwork(sc::INetwork* network)
{
    m_pNetwork.Reset(network);
}

/// @brief 新连接建立回调。
void CDemoService::OnAccept(sc::ConnectionId id, const std::string& peer)
{
    Log("连接建立: id=" + std::to_string(id) + " peer=" + peer);
}

/// @brief 收到数据回调。
///
/// 数据追加到该连接的待解析缓冲，尝试解析完整报文。
void CDemoService::OnData(sc::ConnectionId id, const char* data, size_t len)
{
    std::string& pending = m_mapPendingBuffers[id];
    pending.append(data, len);

    size_t consumed = 0;
    Packet packet;
    while (true)
    {
        // ① 尝试解析一个完整报文
        ParseResult result = CDemoProtocol::ParsePacket(pending, &consumed, &packet);
        if (result == ParseResult::kNeedMore)
        {
            break; // 等待更多数据
        }
        if (result == ParseResult::kInvalid)
        {
            // ② 协议非法，丢弃缓冲并关闭连接
            pending.clear();
            Log("协议错误，关闭连接: id=" + std::to_string(id));
            if (m_pNetwork != nullptr)
            {
                m_pNetwork->Close(id);
            }
            return;
        }
        // ③ 移除已消费的字节并处理报文
        pending.erase(0, consumed);
        HandlePacket(id, packet);
    }
}

/// @brief 连接关闭回调。
void CDemoService::OnClose(sc::ConnectionId id)
{
    Log("连接关闭: id=" + std::to_string(id));
    m_mapPendingBuffers.erase(id);
}

/// @brief 处理一个完整报文。
///
/// 支持 PING（返回 PONG）与 ECHO（返回相同负载）。
void CDemoService::HandlePacket(sc::ConnectionId id, const Packet& packet)
{
    if (packet.command == kCmdPing)
    {
        std::string response = CDemoProtocol::BuildPong();
        Log("收到 PING，返回 PONG: id=" + std::to_string(id));
        if (m_pNetwork != nullptr)
        {
            m_pNetwork->Send(id, response.data(), response.size());
        }
        return;
    }
    if (packet.command == kCmdEcho)
    {
        std::string response = CDemoProtocol::BuildPacket(kCmdEcho, packet.payload);
        Log("收到 ECHO，负载长度=" + std::to_string(packet.payload.size()) + " id=" + std::to_string(id));
        if (m_pNetwork != nullptr)
        {
            m_pNetwork->Send(id, response.data(), response.size());
        }
        return;
    }
    // 未知命令：忽略
    Log("未知命令: " + std::to_string(packet.command) + " id=" + std::to_string(id));
}

/// @brief 接口查询实现。
bool CDemoService::QueryInterfaceImpl(const sc::InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(sc::IID_INetworkHandler()))
    {
        *ppv = static_cast<sc::INetworkHandler*>(this);
        return true;
    }
    return sc::CComponent::QueryInterfaceImpl(iid, ppv);
}

/// @brief 记录日志。
void CDemoService::Log(const std::string& message)
{
    common::CLogger::Instance().Info("[CDemoService] " + message);
}

} // namespace demo
