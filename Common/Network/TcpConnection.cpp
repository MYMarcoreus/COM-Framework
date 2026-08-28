#include "Network/TcpConnection.h"

#include <chrono>

namespace common {
namespace network {

namespace
{
/// 单次读取缓冲大小。
const size_t kReadSize = 4096;
} // namespace

/// @brief 创建 TCP 连接。
///
/// @param io 事件循环（io_context）。
/// @param id 连接标识。
/// @param socket 已建立连接的 socket（从 acceptor 移交）。
CTcpConnection::CTcpConnection(asio::io_context& io, ConnectionId nId, asio::ip::tcp::socket socket)
    : m_io(io),
      m_id(nId),
      m_socket(std::move(socket)),
      m_vecReadBuffer(kReadSize),
      m_bWriting(false),
      m_bClosed(false),
      m_nIdleSeconds(0),
      m_lastActive(std::chrono::steady_clock::now())
{
    asio::error_code ec;
    asio::ip::tcp::endpoint endpointPeer = m_socket.remote_endpoint(ec);
    if (!ec)
    {
        m_strPeerAddress = endpointPeer.address().to_string() + ":" +
                           std::to_string(endpointPeer.port());
    }
}

/// @brief 销毁连接。
CTcpConnection::~CTcpConnection()
{
    asio::error_code ignore;
    static_cast<void>(m_socket.close(ignore));
}

/// @brief 返回连接标识。
ConnectionId CTcpConnection::id() const
{
    return m_id;
}

/// @brief 返回对端地址字符串。
const std::string& CTcpConnection::PeerAddress() const
{
    return m_strPeerAddress;
}

/// @brief 设置数据与关闭回调。
///
/// @param dataCallback 收到数据时回调。
/// @param closeCallback 连接异常关闭时回调。
void CTcpConnection::SetCallbacks(const DataCallback& fnDataCallback, const CloseCallback& fnCloseCallback)
{
    m_fnDataCallback = fnDataCallback;
    m_fnCloseCallback = fnCloseCallback;
}

/// @brief 开始异步读取。
void CTcpConnection::StartRead()
{
    DoRead();
}

/// @brief 发送数据。
///
/// 将发送操作投递到事件循环线程执行，保证与读写回调串行，线程安全。
///
/// @param data 数据起始指针。
/// @param len 数据长度。
void CTcpConnection::Send(const char* pData, size_t nLen)
{
    if (m_bClosed.load())
    {
        return;
    }
    std::string strPayload(pData, nLen);
    Ptr self = shared_from_this();
    asio::post(m_io, [self, strPayload]() { self->AppendWrite(strPayload); });
}

/// @brief 关闭连接。
///
/// 将关闭操作投递到事件循环线程执行，线程安全。
void CTcpConnection::Close()
{
    if (m_bClosed.load())
    {
        return;
    }
    Ptr self = shared_from_this();
    asio::post(m_io, [self]() { self->CloseOnIoThread(); });
}

/// @brief 是否已关闭。
bool CTcpConnection::IsClosed() const
{
    return m_bClosed.load();
}

/// @brief 发起一次异步读。
void CTcpConnection::DoRead()
{
    if (m_bClosed.load())
    {
        return;
    }
    Ptr self = shared_from_this();
    m_socket.async_read_some(asio::buffer(m_vecReadBuffer),
        [self](const asio::error_code& ec, size_t nBytes)
        {
            self->HandleRead(ec, nBytes);
        });
}

/// @brief 处理读完成。
///
/// ① 出错或对端关闭时进入关闭流程。
/// ② 将数据写入输入缓冲并通知上层。
void CTcpConnection::HandleRead(const asio::error_code& ec, size_t nBytes)
{
    // ① 出错或对端关闭
    if (ec || nBytes == 0)
    {
        HandleError(ec);
        return;
    }
    // ② 更新活跃时间（收到数据即活跃）
    m_lastActive = std::chrono::steady_clock::now();

    // ③ 追加数据并通知上层
    m_inputBuffer.Append(&m_vecReadBuffer[0], nBytes);
    if (m_fnDataCallback)
    {
        m_fnDataCallback(shared_from_this(), m_inputBuffer.Peek(), m_inputBuffer.Readable());
    }
    m_inputBuffer.RetrieveAll();
    DoRead();
}

/// @brief 设置空闲超时。
///
/// @param nSeconds 空闲秒数；0 表示不启用空闲检测。
void CTcpConnection::SetIdleTimeout(uint32_t nSeconds)
{
    m_nIdleSeconds = nSeconds;
}

/// @brief 距上次活跃已空闲的秒数。
///
/// @return 空闲秒数。
uint64_t CTcpConnection::IdleSeconds() const
{
    std::chrono::steady_clock::duration elapsed =
        std::chrono::steady_clock::now() - m_lastActive;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
}

/// @brief 追加待发送数据并启动写。
void CTcpConnection::AppendWrite(const std::string& strData)
{
    if (m_bClosed.load())
    {
        return;
    }
    m_strPendingOutput.append(strData);
    if (!m_bWriting.load())
    {
        DoWrite();
    }
}

/// @brief 发起一次异步写。
void CTcpConnection::DoWrite()
{
    m_bWriting.store(true);
    Ptr self = shared_from_this();
    m_socket.async_write_some(asio::buffer(m_strPendingOutput),
        [self](const asio::error_code& ec, size_t nBytes)
        {
            self->HandleWrite(ec, nBytes);
        });
}

/// @brief 处理写完成。
void CTcpConnection::HandleWrite(const asio::error_code& ec, size_t nBytes)
{
    if (ec)
    {
        HandleError(ec);
        return;
    }
    m_strPendingOutput.erase(0, nBytes);
    if (!m_strPendingOutput.empty())
    {
        DoWrite();
    }
    else
    {
        m_bWriting.store(false);
    }
}

/// @brief 在 io 线程内关闭连接。
void CTcpConnection::CloseOnIoThread()
{
    if (m_bClosed.load())
    {
        return;
    }
    m_bClosed.store(true);
    asio::error_code ignore;
    static_cast<void>(m_socket.close(ignore));
}

/// @brief 处理错误或对端关闭。
void CTcpConnection::HandleError(const asio::error_code& ec)
{
    (void)ec;
    if (m_bClosed.load())
    {
        return;
    }
    m_bClosed.store(true);
    asio::error_code ignore;
    static_cast<void>(m_socket.close(ignore));
    if (m_fnCloseCallback)
    {
        m_fnCloseCallback(shared_from_this());
    }
}

} // namespace network
} // namespace common
