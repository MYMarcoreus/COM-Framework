#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>

#include "Common/Types.h"
#include "Network/Acceptor.h"
#include "Network/EventLoop.h"
#include "Network/INetworkHandler.h"
#include "Network/TcpConnection.h"

namespace sc {

/// @brief TCP 服务器。
///
/// 协调 Acceptor、TcpConnection 与 EventLoop，管理连接生命周期。
/// 事件循环在独立线程中运行。
///
/// @note 连接对象由本类持有并负责释放。
class TcpServer
{
public:
    TcpServer();

    ~TcpServer();

    // 启动服务器并监听端口。
    bool Start(uint16_t port, INetworkHandler* handler);

    // 停止服务器，等待事件循环线程退出。
    void Stop();

    // 向指定连接发送数据。
    bool Send(ConnectionId id, const char* data, size_t len);

    // 关闭指定连接。
    void Close(ConnectionId id);

    // 当前监听端口。
    uint16_t ListeningPort() const;

    // 是否正在运行。
    bool IsRunning() const;

private:
    // 事件循环线程入口。
    void ThreadMain();

    // 处理就绪事件。
    void HandleEvent(int fd, short revents);

    // 接受新连接。
    void HandleAccept();

    // 关闭指定连接。
    void HandleClose(TcpConnection* conn);

    // 关闭所有连接。
    void CloseAllConnections();

    Acceptor acceptor_;
    EventLoop loop_;
    INetworkHandler* handler_;
    std::map<ConnectionId, TcpConnection*> connections_;
    std::map<int, ConnectionId> fdToId_;
    std::mutex mutex_;
    std::thread thread_;
    ConnectionId nextId_;
    uint16_t port_;
    std::atomic<bool> running_;
};

} // namespace sc
