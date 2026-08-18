#include "Network/TcpServer.h"

#include <chrono>
#include <functional>
#include <vector>

namespace common {

/// @brief 创建 TCP 服务器。
CTcpServer::CTcpServer()
    : m_nNextId(1), m_nPort(0), m_bRunning(false),
      m_nConnectionCount(0), m_nTotalAccepted(0), m_nTotalClosed(0),
      m_nIdleSeconds(0)
{
}

/// @brief 销毁 TCP 服务器。
CTcpServer::~CTcpServer()
{
    Stop();
}

/// @brief 启动服务器并监听端口。
///
/// @param port 监听端口。
/// @param acceptCb 新连接回调。
/// @param dataCb 数据回调。
/// @param closeCb 连接关闭回调。
///
/// @return 成功返回 true。
bool CTcpServer::Start(uint16_t nPort, const AcceptCallback& fnAccept,
                      const DataCallback& fnData, const CloseCallback& fnClose)
{
    if (m_bRunning.load())
    {
        return false;
    }
    m_fnAccept = fnAccept;
    m_fnData = fnData;
    m_fnClose = fnClose;

    // ① 创建并配置 acceptor
    asio::error_code ec;
    m_pAcceptor.reset(new asio::ip::tcp::acceptor(m_io));
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), nPort);
    static_cast<void>(m_pAcceptor->open(endpoint.protocol(), ec));
    if (!ec)
    {
        static_cast<void>(m_pAcceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true), ec));
    }
    if (!ec)
    {
        static_cast<void>(m_pAcceptor->bind(endpoint, ec));
    }
    if (!ec)
    {
        static_cast<void>(m_pAcceptor->listen(asio::socket_base::max_listen_connections, ec));
    }
    if (ec)
    {
        return false;
    }

    m_nPort = nPort;
    m_bRunning.store(true);
    m_thread = std::thread(&CTcpServer::ThreadMain, this);
    asio::post(m_io, [this]()
    {
        if (m_nIdleSeconds.load() != 0 && m_pIdleTimer == nullptr)
        {
            StartIdleTimer();
        }
    });
    return true;
}

/// @brief 停止服务器。
///
/// 投递关闭任务到事件循环线程，等待线程退出。
void CTcpServer::Stop()
{
    if (!m_bRunning.load())
    {
        return;
    }
    m_bRunning.store(false);
    asio::post(m_io, [this]() { ShutdownOnIoThread(); });
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

/// @brief 事件循环线程入口。
void CTcpServer::ThreadMain()
{
    StartAccept();
    m_io.run();
    // 事件循环退出后兜底关闭剩余连接
    ShutdownOnIoThread();
}

/// @brief 发起一次异步 accept。
void CTcpServer::StartAccept()
{
    if (m_pAcceptor == nullptr || !m_pAcceptor->is_open())
    {
        return;
    }
    m_pAcceptor->async_accept(
        [this](const asio::error_code& ec, asio::ip::tcp::socket socket)
        {
            // ① 错误处理
            if (ec)
            {
                if (ec == asio::error::operation_aborted)
                {
                    return; // 服务器关闭中
                }
                if (m_pAcceptor != nullptr && m_pAcceptor->is_open())
                {
                    StartAccept(); // 瞬时错误，继续接受
                }
                return;
            }
            // ② 创建连接并注册
            ConnectionId nId = m_nNextId++;
            CTcpConnection::Ptr pConn =
                std::make_shared<CTcpConnection>(m_io, nId, std::move(socket));
            pConn->SetCallbacks(
                std::bind(&CTcpServer::HandleData, this, std::placeholders::_1,
                          std::placeholders::_2, std::placeholders::_3),
                std::bind(&CTcpServer::HandleClose, this, std::placeholders::_1));
            m_mapConnections[nId] = pConn;
            pConn->StartRead();
            {
                std::lock_guard<std::mutex> lock(m_mutexPeer);
                m_mapPeerAddresses[nId] = pConn->PeerAddress();
            }
            m_nConnectionCount.fetch_add(1);
            m_nTotalAccepted.fetch_add(1);
            if (m_fnAccept)
            {
                m_fnAccept(nId, pConn->PeerAddress());
            }
            StartAccept(); // 继续接受下一个连接
        });
}

/// @brief 连接数据回调。
void CTcpServer::HandleData(const CTcpConnection::Ptr& pConn, const char* pData, size_t nLen)
{
    if (m_fnData)
    {
        m_fnData(pConn->id(), pData, nLen);
    }
}

/// @brief 连接关闭回调。
///
/// 从连接管理表中移除连接，并通知上层。
void CTcpServer::HandleClose(const CTcpConnection::Ptr& pConn)
{
    ConnectionId nId = pConn->id();
    m_mapConnections.erase(nId);
    {
        std::lock_guard<std::mutex> lock(m_mutexPeer);
        m_mapPeerAddresses.erase(nId);
    }
    if (m_nConnectionCount.load() > 0)
    {
        m_nConnectionCount.fetch_sub(1);
    }
    m_nTotalClosed.fetch_add(1);
    if (m_fnClose)
    {
        m_fnClose(nId);
    }
}

/// @brief 向指定连接发送数据。
///
/// 查找与投递均在事件循环线程执行，线程安全。
bool CTcpServer::Send(ConnectionId nId, const char* pData, size_t nLen)
{
    if (!m_bRunning.load())
    {
        return false;
    }
    std::string strPayload(pData, nLen);
    asio::post(m_io, [this, nId, strPayload]()
    {
        std::map<ConnectionId, CTcpConnection::Ptr>::iterator it = m_mapConnections.find(nId);
        if (it != m_mapConnections.end())
        {
            it->second->Send(strPayload.data(), strPayload.size());
        }
    });
    return true;
}

/// @brief 关闭指定连接。
void CTcpServer::Close(ConnectionId nId)
{
    asio::post(m_io, [this, nId]()
    {
        std::map<ConnectionId, CTcpConnection::Ptr>::iterator it = m_mapConnections.find(nId);
        if (it == m_mapConnections.end())
        {
            return;
        }
        CTcpConnection::Ptr pConn = it->second;
        m_mapConnections.erase(it);
        {
            std::lock_guard<std::mutex> lock(m_mutexPeer);
            m_mapPeerAddresses.erase(nId);
        }
        if (m_nConnectionCount.load() > 0)
        {
            m_nConnectionCount.fetch_sub(1);
        }
        m_nTotalClosed.fetch_add(1);
        pConn->Close();
        if (m_fnClose)
        {
            m_fnClose(nId);
        }
    });
}

/// @brief 设置空闲超时。
///
/// @param nSeconds 空闲秒数；0 表示禁用空闲检测。
void CTcpServer::SetIdleTimeout(uint32_t nSeconds)
{
    m_nIdleSeconds.store(nSeconds);
    if (nSeconds != 0)
    {
        asio::post(m_io, [this]()
        {
            if (m_pIdleTimer == nullptr)
            {
                StartIdleTimer();
            }
        });
    }
}

/// @brief 启动空闲检测定时器。
///
/// 每秒触发一次，扫描并关闭空闲超时的连接；服务器停止时取消。
void CTcpServer::StartIdleTimer()
{
    if (m_nIdleSeconds.load() == 0)
    {
        return;
    }
    m_pIdleTimer.reset(new asio::steady_timer(m_io));
    m_pIdleTimer->expires_after(std::chrono::seconds(1));
    m_pIdleTimer->async_wait(
        [this](const asio::error_code& ec)
        {
            if (ec)
            {
                return; // 定时器被取消（服务器关闭中）
            }
            CheckIdleConnections();
            if (m_bRunning.load())
            {
                StartIdleTimer(); // 周期继续
            }
        });
}

/// @brief 扫描并关闭空闲超时的连接。
///
/// 仅扫描，关闭通过 Close 投递到事件循环线程执行。
void CTcpServer::CheckIdleConnections()
{
    uint32_t nIdle = m_nIdleSeconds.load();
    if (nIdle == 0)
    {
        return;
    }
    std::vector<ConnectionId> vecIdle;
    for (std::map<ConnectionId, CTcpConnection::Ptr>::const_iterator it = m_mapConnections.begin();
         it != m_mapConnections.end(); ++it)
    {
        if (it->second->IdleSeconds() >= nIdle)
        {
            vecIdle.push_back(it->first);
        }
    }
    for (size_t i = 0; i < vecIdle.size(); ++i)
    {
        Close(vecIdle[i]);
    }
}

/// @brief 在 io 线程内执行关闭流程。
///
/// 关闭 acceptor 与所有连接，幂等可重复调用。
void CTcpServer::ShutdownOnIoThread()
{
    if (m_pIdleTimer != nullptr)
    {
        static_cast<void>(m_pIdleTimer->cancel());
        m_pIdleTimer.reset();
    }
    if (m_pAcceptor != nullptr && m_pAcceptor->is_open())
    {
        asio::error_code ignore;
        static_cast<void>(m_pAcceptor->close(ignore));
    }
    std::vector<CTcpConnection::Ptr> vecAll;
    for (std::map<ConnectionId, CTcpConnection::Ptr>::iterator it = m_mapConnections.begin();
         it != m_mapConnections.end(); ++it)
    {
        vecAll.push_back(it->second);
    }
    m_mapConnections.clear();
    {
        std::lock_guard<std::mutex> lock(m_mutexPeer);
        m_mapPeerAddresses.clear();
    }
    m_nConnectionCount.store(0);
    for (size_t i = 0; i < vecAll.size(); ++i)
    {
        vecAll[i]->Close();
        m_nTotalClosed.fetch_add(1);
        if (m_fnClose)
        {
            m_fnClose(vecAll[i]->id());
        }
    }
}

/// @brief 返回当前监听端口。
uint16_t CTcpServer::ListeningPort() const
{
    return m_nPort;
}

/// @brief 是否正在运行。
bool CTcpServer::IsRunning() const
{
    return m_bRunning.load();
}

/// @brief 当前活跃连接数。
size_t CTcpServer::ConnectionCount() const
{
    return m_nConnectionCount.load();
}

/// @brief 累计接受连接数。
uint64_t CTcpServer::TotalAccepted() const
{
    return m_nTotalAccepted.load();
}

/// @brief 累计关闭连接数。
uint64_t CTcpServer::TotalClosed() const
{
    return m_nTotalClosed.load();
}

/// @brief 指定连接是否存在。
bool CTcpServer::HasConnection(ConnectionId nId) const
{
    std::lock_guard<std::mutex> lock(m_mutexPeer);
    return m_mapPeerAddresses.find(nId) != m_mapPeerAddresses.end();
}

/// @brief 指定连接的对端地址。
///
/// @param nId 连接标识。
///
/// @return 对端地址字符串；连接不存在时返回空串。
std::string CTcpServer::PeerAddress(ConnectionId nId) const
{
    std::lock_guard<std::mutex> lock(m_mutexPeer);
    std::map<ConnectionId, std::string>::const_iterator it = m_mapPeerAddresses.find(nId);
    if (it == m_mapPeerAddresses.end())
    {
        return "";
    }
    return it->second;
}

} // namespace common
