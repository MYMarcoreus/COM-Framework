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
namespace network {

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
    bool Start(uint16_t nPort, const AcceptCallback& fnAccept,
               const DataCallback& fnData, const CloseCallback& fnClose);

    // 停止服务器，等待事件循环线程退出。
    void Stop();

    // 向指定连接发送数据。
    bool Send(ConnectionId nId, const char* pData, size_t nLen);

    // 关闭指定连接。
    void Close(ConnectionId nId);

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
    bool HasConnection(ConnectionId nId) const;

    // 指定连接的对端地址；连接不存在时返回空串。
    std::string PeerAddress(ConnectionId nId) const;

    // 设置空闲超时（秒，0 表示禁用）。周期扫描并自动关闭空闲超时连接。
    void SetIdleTimeout(uint32_t nSeconds);

    // 设置最大连接数上限（0 表示不限制）；达到上限时新连接被直接关闭。
    void SetMaxConnections(size_t nMax);

private:
    // 启动空闲检测定时器（事件循环线程内调用）。
    void StartIdleTimer();

    // 扫描并关闭空闲超时的连接（事件循环线程内调用）。
    void CheckIdleConnections();
    // 事件循环线程入口。
    void ThreadMain();

    // 发起一次异步 accept。
    void StartAccept();

    // 处理 accept 完成。
    void HandleAccept(const asio::error_code& ec, asio::ip::tcp::socket socket);

    // 连接数据回调。
    void HandleData(const CTcpConnection::Ptr& pConn, const char* pData, size_t nLen);

    // 连接关闭回调。
    void HandleClose(const CTcpConnection::Ptr& pConn);

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
    std::atomic<uint32_t> m_nIdleSeconds;
    std::atomic<size_t> m_nMaxConnections;
    std::unique_ptr<asio::steady_timer> m_pIdleTimer;
};

} // namespace network
} // namespace common
