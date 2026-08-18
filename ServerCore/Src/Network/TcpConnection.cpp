#include "Network/TcpConnection.h"

#include <cstdint>

namespace sc {

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
TcpConnection::TcpConnection(asio::io_context& io, ConnectionId id, asio::ip::tcp::socket socket)
    : io_(io),
      id_(id),
      socket_(std::move(socket)),
      readBuffer_(kReadSize),
      writing_(false),
      closed_(false)
{
    asio::error_code ec;
    asio::ip::tcp::endpoint peer = socket_.remote_endpoint(ec);
    if (!ec)
    {
        peerAddress_ = peer.address().to_string() + ":" + std::to_string(peer.port());
    }
}

/// @brief 销毁连接。
TcpConnection::~TcpConnection()
{
    asio::error_code ignore;
    socket_.close(ignore);
}

/// @brief 返回连接标识。
ConnectionId TcpConnection::id() const
{
    return id_;
}

/// @brief 返回对端地址字符串。
const std::string& TcpConnection::PeerAddress() const
{
    return peerAddress_;
}

/// @brief 设置数据与关闭回调。
///
/// @param dataCallback 收到数据时回调。
/// @param closeCallback 连接异常关闭时回调。
void TcpConnection::SetCallbacks(const DataCallback& dataCallback, const CloseCallback& closeCallback)
{
    dataCallback_ = dataCallback;
    closeCallback_ = closeCallback;
}

/// @brief 开始异步读取。
void TcpConnection::StartRead()
{
    DoRead();
}

/// @brief 发送数据。
///
/// 将发送操作投递到事件循环线程执行，保证与读写回调串行，线程安全。
///
/// @param data 数据起始指针。
/// @param len 数据长度。
void TcpConnection::Send(const char* data, size_t len)
{
    if (closed_.load())
    {
        return;
    }
    std::string payload(data, len);
    Ptr self = shared_from_this();
    asio::post(io_, [self, payload]() { self->AppendWrite(payload); });
}

/// @brief 关闭连接。
///
/// 将关闭操作投递到事件循环线程执行，线程安全。
void TcpConnection::Close()
{
    if (closed_.load())
    {
        return;
    }
    Ptr self = shared_from_this();
    asio::post(io_, [self]() { self->CloseOnIoThread(); });
}

/// @brief 是否已关闭。
bool TcpConnection::IsClosed() const
{
    return closed_.load();
}

/// @brief 发起一次异步读。
void TcpConnection::DoRead()
{
    if (closed_.load())
    {
        return;
    }
    Ptr self = shared_from_this();
    socket_.async_read_some(asio::buffer(readBuffer_),
        [self](const asio::error_code& ec, size_t bytes)
        {
            self->HandleRead(ec, bytes);
        });
}

/// @brief 处理读完成。
///
/// ① 出错或对端关闭时进入关闭流程。
/// ② 将数据写入输入缓冲并通知上层。
void TcpConnection::HandleRead(const asio::error_code& ec, size_t bytes)
{
    // ① 出错或对端关闭
    if (ec || bytes == 0)
    {
        HandleError(ec);
        return;
    }
    // ② 追加数据并通知上层
    inputBuffer_.Append(&readBuffer_[0], bytes);
    if (dataCallback_)
    {
        dataCallback_(shared_from_this(), inputBuffer_.Peek(), inputBuffer_.Readable());
    }
    inputBuffer_.RetrieveAll();
    DoRead();
}

/// @brief 追加待发送数据并启动写。
void TcpConnection::AppendWrite(const std::string& data)
{
    if (closed_.load())
    {
        return;
    }
    pendingOutput_.append(data);
    if (!writing_.load())
    {
        DoWrite();
    }
}

/// @brief 发起一次异步写。
void TcpConnection::DoWrite()
{
    writing_.store(true);
    Ptr self = shared_from_this();
    socket_.async_write_some(asio::buffer(pendingOutput_),
        [self](const asio::error_code& ec, size_t bytes)
        {
            self->HandleWrite(ec, bytes);
        });
}

/// @brief 处理写完成。
void TcpConnection::HandleWrite(const asio::error_code& ec, size_t bytes)
{
    if (ec)
    {
        HandleError(ec);
        return;
    }
    pendingOutput_.erase(0, bytes);
    if (!pendingOutput_.empty())
    {
        DoWrite();
    }
    else
    {
        writing_.store(false);
    }
}

/// @brief 在 io 线程内关闭连接。
void TcpConnection::CloseOnIoThread()
{
    if (closed_.load())
    {
        return;
    }
    closed_.store(true);
    asio::error_code ignore;
    socket_.close(ignore);
}

/// @brief 处理错误或对端关闭。
void TcpConnection::HandleError(const asio::error_code& ec)
{
    (void)ec;
    if (closed_.load())
    {
        return;
    }
    closed_.store(true);
    asio::error_code ignore;
    socket_.close(ignore);
    if (closeCallback_)
    {
        closeCallback_(shared_from_this());
    }
}

} // namespace sc
