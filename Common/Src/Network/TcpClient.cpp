#include "Network/TcpClient.h"

#include <functional>

namespace common {

/// @brief 创建 TCP 客户端。
CTcpClient::CTcpClient()
    : m_socket(m_io), m_resolver(m_io), m_nPort(0), m_bRunning(false),
      m_bConnected(false), m_bCloseNotified(false)
{
    m_vecReadBuffer.resize(8192);
}

/// @brief 销毁 TCP 客户端。
CTcpClient::~CTcpClient()
{
    Stop();
}

/// @brief 异步连接远程服务器。
///
/// @param host 远程主机名或 IP。
/// @param port 远程端口。
/// @param connectCb 连接结果回调。
/// @param dataCb 数据回调。
/// @param closeCb 连接关闭回调。
///
/// @return 成功发起连接返回 true。
bool CTcpClient::Connect(const std::string& host, uint16_t port,
                        const ConnectCallback& connectCb, const DataCallback& dataCb,
                        const CloseCallback& closeCb)
{
    if (m_bRunning.load())
    {
        return false; // 已在连接中
    }
    m_strHost = host;
    m_nPort = port;
    m_fnConnect = connectCb;
    m_fnData = dataCb;
    m_fnClose = closeCb;
    m_bConnected.store(false);
    m_bCloseNotified.store(false);
    m_bRunning.store(true);
    m_thread = std::thread(&CTcpClient::ThreadMain, this);
    return true;
}

/// @brief 发送数据。
///
/// 投递到事件循环线程写入，线程安全。
///
/// @return 已连接时返回 true。
bool CTcpClient::Send(const char* data, size_t len)
{
    if (!m_bConnected.load() || data == nullptr || len == 0)
    {
        return false;
    }
    std::string payload(data, len);
    asio::post(m_io, [this, payload]() { AppendWrite(payload); });
    return true;
}

/// @brief 关闭连接。
///
/// 投递到事件循环线程关闭，线程安全。
void CTcpClient::Close()
{
    if (!m_bRunning.load())
    {
        return;
    }
    asio::post(m_io, [this]() { CloseOnIoThread(); });
}

/// @brief 停止客户端。
///
/// 投递关闭任务到事件循环线程并等待线程退出。
/// 若线程因连接失败/关闭已自然退出，仍执行 join 避免未 join 线程析构。
void CTcpClient::Stop()
{
    if (m_bRunning.load())
    {
        m_bRunning.store(false);
        asio::post(m_io, [this]() { CloseOnIoThread(); });
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

/// @brief 是否已连接。
bool CTcpClient::IsConnected() const
{
    return m_bConnected.load();
}

/// @brief 对端地址字符串。
std::string CTcpClient::PeerAddress() const
{
    return m_strPeerAddress;
}

/// @brief 事件循环线程入口。
void CTcpClient::ThreadMain()
{
    StartConnect();
    m_io.run();
    m_bRunning.store(false);
}

/// @brief 发起异步连接。
///
/// 先解析主机名，再异步连接。
void CTcpClient::StartConnect()
{
    m_resolver.async_resolve(m_strHost, std::to_string(m_nPort),
        [this](const asio::error_code& ec, asio::ip::tcp::resolver::results_type results)
        {
            if (ec)
            {
                // 解析失败：通知连接失败
                if (m_fnConnect)
                {
                    m_fnConnect(false, "");
                }
                NotifyClose();
                return;
            }
            asio::async_connect(m_socket, results,
                [this](const asio::error_code& cerr,
                       const asio::ip::tcp::endpoint& endpoint)
                {
                    HandleConnect(cerr, endpoint);
                });
        });
}

/// @brief 处理连接完成。
void CTcpClient::HandleConnect(const asio::error_code& ec, const asio::ip::tcp::endpoint& endpoint)
{
    if (ec)
    {
        if (m_fnConnect)
        {
            m_fnConnect(false, "");
        }
        NotifyClose();
        return;
    }
    m_strPeerAddress = endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    m_bConnected.store(true);
    if (m_fnConnect)
    {
        m_fnConnect(true, m_strPeerAddress);
    }
    DoRead();
}

/// @brief 发起一次异步读。
void CTcpClient::DoRead()
{
    m_socket.async_read_some(asio::buffer(m_vecReadBuffer),
        [this](const asio::error_code& ec, size_t bytes)
        {
            HandleRead(ec, bytes);
        });
}

/// @brief 处理读完成。
void CTcpClient::HandleRead(const asio::error_code& ec, size_t bytes)
{
    if (ec)
    {
        if (ec != asio::error::operation_aborted)
        {
            NotifyClose();
        }
        return;
    }
    m_inputBuffer.Append(m_vecReadBuffer.data(), bytes);
    if (m_fnData)
    {
        m_fnData(m_inputBuffer.Peek(), m_inputBuffer.Readable());
    }
    m_inputBuffer.RetrieveAll();
    if (!m_bConnected.load())
    {
        return;
    }
    DoRead();
}

/// @brief 追加待发送数据并启动写。
void CTcpClient::AppendWrite(const std::string& data)
{
    if (!m_socket.is_open())
    {
        return;
    }
    bool writing = !m_strPendingOutput.empty();
    m_strPendingOutput.append(data);
    if (!writing)
    {
        DoWrite();
    }
}

/// @brief 发起一次异步写。
void CTcpClient::DoWrite()
{
    m_socket.async_write_some(asio::buffer(m_strPendingOutput),
        [this](const asio::error_code& ec, size_t bytes)
        {
            HandleWrite(ec, bytes);
        });
}

/// @brief 处理写完成。
void CTcpClient::HandleWrite(const asio::error_code& ec, size_t bytes)
{
    if (ec)
    {
        if (ec != asio::error::operation_aborted)
        {
            NotifyClose();
        }
        return;
    }
    m_strPendingOutput.erase(0, bytes);
    if (!m_strPendingOutput.empty())
    {
        DoWrite();
    }
}

/// @brief 在 io 线程内关闭连接。
void CTcpClient::CloseOnIoThread()
{
    if (m_socket.is_open())
    {
        asio::error_code ignored;
        static_cast<void>(m_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored));
        static_cast<void>(m_socket.close(ignored));
    }
    m_bConnected.store(false);
}

/// @brief 通知上层连接关闭（仅一次）。
void CTcpClient::NotifyClose()
{
    bool expected = false;
    if (m_bCloseNotified.compare_exchange_strong(expected, true))
    {
        if (m_fnClose)
        {
            m_fnClose();
        }
    }
}

} // namespace common
