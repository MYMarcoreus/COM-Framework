#include "Network/NetworkComponent.h"

#include <string>

#include "Network/TcpServer.h"

namespace sc {

/// @brief 创建网络组件。
NetworkComponent::NetworkComponent() : port_(0)
{
}

/// @brief 销毁网络组件。
NetworkComponent::~NetworkComponent()
{
    Stop();
}

/// @brief 启动 TCP 服务器。
///
/// @param port 监听端口。
/// @param handler 网络事件处理器。
///
/// @return 成功返回 true。
bool NetworkComponent::StartTcpServer(uint16_t port, INetworkHandler* handler)
{
    if (handler == nullptr)
    {
        return false;
    }
    // 若已有运行中的服务器，先停止
    Stop();

    // 持有 handler 引用，确保网络运行期间 handler 存活
    handler_.Reset(handler);

    // 将组件接口回调适配为 Common TcpServer 的 std::function 回调
    std::unique_ptr<common::TcpServer> newServer(new common::TcpServer());
    common::TcpServer::AcceptCallback acceptCb =
        [this](common::ConnectionId id, const std::string& peer)
        {
            if (handler_ != nullptr)
            {
                handler_->OnAccept(id, peer);
            }
        };
    common::TcpServer::DataCallback dataCb =
        [this](common::ConnectionId id, const char* data, size_t len)
        {
            if (handler_ != nullptr)
            {
                handler_->OnData(id, data, len);
            }
        };
    common::TcpServer::CloseCallback closeCb =
        [this](common::ConnectionId id)
        {
            if (handler_ != nullptr)
            {
                handler_->OnClose(id);
            }
        };
    if (!newServer->Start(port, acceptCb, dataCb, closeCb))
    {
        handler_.Reset();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        server_.swap(newServer);
        port_ = port;
    }
    return true;
}

/// @brief 停止服务器。
///
/// 先释放 server 所有权，再在锁外停止，避免等待事件循环线程时死锁。
void NetworkComponent::Stop()
{
    common::TcpServer* server = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        server = server_.release();
        port_ = 0;
    }
    if (server != nullptr)
    {
        server->Stop(); // 等待事件循环线程退出（不持有本组件锁）
        delete server;
    }
    handler_.Reset();
}

/// @brief 向指定连接发送数据。
bool NetworkComponent::Send(ConnectionId id, const char* data, size_t len)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (server_ == nullptr)
    {
        return false;
    }
    return server_->Send(id, data, len);
}

/// @brief 关闭指定连接。
void NetworkComponent::Close(ConnectionId id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (server_ != nullptr)
    {
        server_->Close(id);
    }
}

/// @brief 返回当前监听端口。
uint16_t NetworkComponent::ListeningPort() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return port_;
}

/// @brief 接口查询实现。
bool NetworkComponent::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_INetwork()))
    {
        *ppv = static_cast<INetwork*>(this);
        return true;
    }
    return Component::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
