#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "asio.hpp"

#include "Network/NetworkTypes.h"
#include "Network/TcpConnection.h"

namespace common {

/// @brief TCP 服务器（基于 asio）。
///
/// 基于 asio::io_context + tcp::acceptor 提供 TCP 服务器能力。
/// 事件循环在独立线程中运行；连接生命周期由 shared_ptr 管理。
/// 通过回调向上层报告 accept / data / close 事件，不依赖具体业务接口。
class CTcpServer
{
public:
    // 新连接回调。
    using AcceptCallback = std::function<void(ConnectionId id, const std::string& peer)>;

    // 数据回调。
    using DataCallback = std::function<void(ConnectionId id, const char* data, size_t len)>;

    // 连接关闭回调。
    using CloseCallback = std::function<void(ConnectionId id)>;

    CTcpServer();

    ~CTcpServer();

    // 启动服务器并监听端口。
    bool Start(uint16_t port, const AcceptCallback& acceptCb,
               const DataCallback& dataCb, const CloseCallback& closeCb);

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

    // 当前活跃连接数。
    size_t ConnectionCount() const;

    // 累计接受连接数。
    uint64_t TotalAccepted() const;

    // 累计关闭连接数。
    uint64_t TotalClosed() const;

    // 指定连接是否存在。
    bool HasConnection(ConnectionId id) const;

    // 指定连接的对端地址；连接不存在时返回空串。
    std::string PeerAddress(ConnectionId id) const;

private:
    // 事件循环线程入口。
    void ThreadMain();

    // 发起一次异步 accept。
    void StartAccept();

    // 处理 accept 完成。
    void HandleAccept(const asio::error_code& ec, asio::ip::tcp::socket socket);

    // 连接数据回调。
    void HandleData(const CTcpConnection::Ptr& conn, const char* data, size_t len);

    // 连接关闭回调。
    void HandleClose(const CTcpConnection::Ptr& conn);

    // 在 io 线程内执行关闭流程。
    void ShutdownOnIoThread();

    asio::io_context m_io;
    std::unique_ptr<asio::ip::tcp::acceptor> m_pAcceptor;
    AcceptCallback m_fnAccept;
    DataCallback m_fnData;
    CloseCallback m_fnClose;
    std::map<ConnectionId, CTcpConnection::Ptr> m_mapConnections;
    std::thread m_thread;
    ConnectionId m_nNextId;
    uint16_t m_nPort;
    std::atomic<bool> m_bRunning;
    std::atomic<size_t> m_nConnectionCount;
    std::atomic<uint64_t> m_nTotalAccepted;
    std::atomic<uint64_t> m_nTotalClosed;
    mutable std::mutex m_mutexPeer;
    std::map<ConnectionId, std::string> m_mapPeerAddresses;
};

} // namespace common
