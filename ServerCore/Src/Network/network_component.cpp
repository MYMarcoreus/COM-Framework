#include "Network/network_component.h"

#include <string>

#include "Network/tcp_server.h"

namespace sc {

/// @brief 创建网络组件。
CNetworkComponent::CNetworkComponent() : m_nPort(0)
{
}

/// @brief 销毁网络组件。
CNetworkComponent::~CNetworkComponent()
{
    Stop();
}

/// @brief 启动 TCP 服务器。
///
/// @param port 监听端口。
/// @param handler 网络事件处理器。
///
/// @return 成功返回 true。
bool CNetworkComponent::StartTcpServer(uint16_t port, INetworkHandler* handler)
{
    if (handler == nullptr)
    {
        return false;
    }
    // 若已有运行中的服务器，先停止
    Stop();

    // 持有 handler 引用，确保网络运行期间 handler 存活
    m_pHandler.Reset(handler);

    // 将组件接口回调适配为 Common CTcpServer 的 std::function 回调
    std::unique_ptr<common::CTcpServer> newServer(new common::CTcpServer());
    common::CTcpServer::AcceptCallback acceptCb =
        [this](common::ConnectionId id, const std::string& peer)
        {
            if (m_pHandler != nullptr)
            {
                m_pHandler->OnAccept(id, peer);
            }
        };
    common::CTcpServer::DataCallback dataCb =
        [this](common::ConnectionId id, const char* data, size_t len)
        {
            if (m_pHandler != nullptr)
            {
                m_pHandler->OnData(id, data, len);
            }
        };
    common::CTcpServer::CloseCallback closeCb =
        [this](common::ConnectionId id)
        {
            if (m_pHandler != nullptr)
            {
                m_pHandler->OnClose(id);
            }
        };
    if (!newServer->Start(port, acceptCb, dataCb, closeCb))
    {
        m_pHandler.Reset();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pServer.swap(newServer);
        m_nPort = port;
    }
    return true;
}

/// @brief 停止服务器。
///
/// 先释放 server 所有权，再在锁外停止，避免等待事件循环线程时死锁。
void CNetworkComponent::Stop()
{
    common::CTcpServer* server = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        server = m_pServer.release();
        m_nPort = 0;
    }
    if (server != nullptr)
    {
        server->Stop(); // 等待事件循环线程退出（不持有本组件锁）
        delete server;
    }
    m_pHandler.Reset();
}

/// @brief 向指定连接发送数据。
bool CNetworkComponent::Send(ConnectionId id, const char* data, size_t len)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pServer == nullptr)
    {
        return false;
    }
    return m_pServer->Send(id, data, len);
}

/// @brief 关闭指定连接。
void CNetworkComponent::Close(ConnectionId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pServer != nullptr)
    {
        m_pServer->Close(id);
    }
}

/// @brief 返回当前监听端口。
uint16_t CNetworkComponent::ListeningPort() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nPort;
}

/// @brief 当前活跃连接数。
size_t CNetworkComponent::ConnectionCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_pServer != nullptr) ? m_pServer->ConnectionCount() : 0;
}

/// @brief 累计接受连接数。
uint64_t CNetworkComponent::TotalAccepted() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_pServer != nullptr) ? m_pServer->TotalAccepted() : 0;
}

/// @brief 累计关闭连接数。
uint64_t CNetworkComponent::TotalClosed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_pServer != nullptr) ? m_pServer->TotalClosed() : 0;
}

/// @brief 指定连接是否存在。
bool CNetworkComponent::HasConnection(ConnectionId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pServer != nullptr && m_pServer->HasConnection(id);
}

/// @brief 指定连接的对端地址。
std::string CNetworkComponent::PeerAddress(ConnectionId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_pServer != nullptr) ? m_pServer->PeerAddress(id) : "";
}

/// @brief 接口查询实现。
bool CNetworkComponent::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_INetwork()))
    {
        *ppv = static_cast<INetwork*>(this);
        return true;
    }
    return CComponent::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
