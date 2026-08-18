#include "Network/TcpServer.h"

#include <cstring>
#include <functional>
#include <poll.h>
#include <vector>

namespace sc {

/// @brief 创建 TCP 服务器。
TcpServer::TcpServer()
    : handler_(nullptr), nextId_(1), port_(0), running_(false)
{
}

/// @brief 销毁 TCP 服务器。
TcpServer::~TcpServer()
{
    Stop();
}

/// @brief 启动服务器并监听端口。
///
/// @param port 监听端口。
/// @param handler 网络事件处理器（借用指针，不持有）。
///
/// @return 成功返回 true。
bool TcpServer::Start(uint16_t port, INetworkHandler* handler)
{
    if (running_.load())
    {
        return false;
    }
    handler_ = handler;
    if (!acceptor_.BindAndListen(port))
    {
        return false;
    }
    port_ = port;
    running_.store(true);
    thread_ = std::thread(&TcpServer::ThreadMain, this);
    return true;
}

/// @brief 停止服务器。
///
/// 停止事件循环并等待线程退出，然后关闭所有连接。
void TcpServer::Stop()
{
    if (!running_.load())
    {
        return;
    }
    running_.store(false);
    loop_.Stop();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

/// @brief 事件循环线程入口。
void TcpServer::ThreadMain()
{
    loop_.SetEventCallback(std::bind(&TcpServer::HandleEvent, this,
                                     std::placeholders::_1, std::placeholders::_2));
    loop_.AddFd(acceptor_.fd(), POLLIN);
    loop_.Run();
    CloseAllConnections();
}

/// @brief 处理就绪事件。
void TcpServer::HandleEvent(int fd, short revents)
{
    // ① 监听套接字可读：接受新连接
    if (fd == acceptor_.fd())
    {
        HandleAccept();
        return;
    }

    // ② 查找对应连接
    TcpConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<int, ConnectionId>::iterator it = fdToId_.find(fd);
        if (it != fdToId_.end())
        {
            std::map<ConnectionId, TcpConnection*>::iterator cit = connections_.find(it->second);
            if (cit != connections_.end())
            {
                conn = cit->second;
            }
        }
    }
    if (conn == nullptr)
    {
        return;
    }

    // ③ 异常事件：关闭连接
    if (revents & (POLLHUP | POLLERR | POLLNVAL))
    {
        HandleClose(conn);
        return;
    }

    // ④ 可读：读取数据并通知上层
    if (revents & POLLIN)
    {
        ssize_t n = conn->Read();
        if (n > 0)
        {
            if (handler_ != nullptr)
            {
                handler_->OnData(conn->id(), conn->InputPeek(), conn->InputReadable());
            }
            conn->RetrieveInput(conn->InputReadable());
        }
        else if (n == 0)
        {
            HandleClose(conn);
            return;
        }
        else
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                HandleClose(conn);
                return;
            }
        }
    }

    // ⑤ 可写：刷新输出缓冲
    if (revents & POLLOUT)
    {
        if (conn->Flush() < 0)
        {
            HandleClose(conn);
            return;
        }
        if (!conn->HasPendingOutput())
        {
            loop_.UpdateEvents(conn->fd(), POLLIN);
        }
    }
}

/// @brief 接受新连接。
void TcpServer::HandleAccept()
{
    while (true)
    {
        sockaddr_in peer;
        std::memset(&peer, 0, sizeof(peer));
        int fd = acceptor_.Accept(&peer);
        if (fd < 0)
        {
            break; // EAGAIN 表示已无待处理连接
        }
        Socket newSocket(fd);
        newSocket.SetNonBlocking();
        newSocket.SetNoDelay();

        ConnectionId id = nextId_++;
        TcpConnection* conn = new TcpConnection(id, newSocket.Release(), peer);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connections_[id] = conn;
            fdToId_[conn->fd()] = id;
        }
        loop_.AddFd(conn->fd(), POLLIN);
        if (handler_ != nullptr)
        {
            handler_->OnAccept(id, conn->PeerAddress());
        }
    }
}

/// @brief 关闭指定连接。
///
/// 幂等：若连接已不在管理表中，说明已关闭，直接返回，避免重复通知 OnClose。
void TcpServer::HandleClose(TcpConnection* conn)
{
    if (conn == nullptr)
    {
        return;
    }
    ConnectionId id = conn->id();
    int fd = conn->fd();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<ConnectionId, TcpConnection*>::iterator it = connections_.find(id);
        if (it == connections_.end() || it->second != conn)
        {
            return; // 已关闭，防止重复通知
        }
        connections_.erase(it);
        fdToId_.erase(fd);
    }
    loop_.RemoveFd(fd);
    if (handler_ != nullptr)
    {
        handler_->OnClose(id);
    }
    delete conn;
}

/// @brief 关闭所有连接。
void TcpServer::CloseAllConnections()
{
    std::vector<TcpConnection*> all;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::map<ConnectionId, TcpConnection*>::iterator it = connections_.begin();
             it != connections_.end(); ++it)
        {
            all.push_back(it->second);
        }
        connections_.clear();
        fdToId_.clear();
    }
    for (size_t i = 0; i < all.size(); ++i)
    {
        if (handler_ != nullptr)
        {
            handler_->OnClose(all[i]->id());
        }
        delete all[i];
    }
}

/// @brief 向指定连接发送数据。
bool TcpServer::Send(ConnectionId id, const char* data, size_t len)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<ConnectionId, TcpConnection*>::iterator it = connections_.find(id);
    if (it == connections_.end())
    {
        return false;
    }
    TcpConnection* conn = it->second;
    conn->Send(data, len);
    if (conn->HasPendingOutput())
    {
        loop_.UpdateEvents(conn->fd(), POLLIN | POLLOUT);
    }
    return true;
}

/// @brief 关闭指定连接。
void TcpServer::Close(ConnectionId id)
{
    TcpConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<ConnectionId, TcpConnection*>::iterator it = connections_.find(id);
        if (it != connections_.end())
        {
            conn = it->second;
        }
    }
    if (conn != nullptr)
    {
        HandleClose(conn);
    }
}

/// @brief 返回当前监听端口。
uint16_t TcpServer::ListeningPort() const
{
    return port_;
}

/// @brief 是否正在运行。
bool TcpServer::IsRunning() const
{
    return running_.load();
}

} // namespace sc
