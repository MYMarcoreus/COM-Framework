#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <thread>

#include "asio.hpp"

#include "Common/Types.h"
#include "Network/INetworkHandler.h"
#include "Network/TcpConnection.h"

namespace sc {

/// @brief TCP 服务器（基于 asio）。
///
/// 基于 asio::io_context + tcp::acceptor 提供 TCP 服务器能力。
/// 事件循环在独立线程中运行；连接生命周期由 shared_ptr 管理。
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

    // 发起一次异步 accept。
    void StartAccept();

    // 处理 accept 完成。
    void HandleAccept(const asio::error_code& ec, asio::ip::tcp::socket socket);

    // 连接数据回调。
    void HandleData(const TcpConnection::Ptr& conn, const char* data, size_t len);

    // 连接关闭回调。
    void HandleClose(const TcpConnection::Ptr& conn);

    // 在 io 线程内执行关闭流程。
    void ShutdownOnIoThread();

    asio::io_context io_;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    INetworkHandler* handler_;
    std::map<ConnectionId, TcpConnection::Ptr> connections_;
    std::thread thread_;
    ConnectionId nextId_;
    uint16_t port_;
    std::atomic<bool> running_;
};

} // namespace sc
