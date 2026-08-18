#include "Network/TcpConnection.h"

#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

namespace sc {

namespace
{
/// 单次读取缓冲大小。
const size_t kReadSize = 4096;
} // namespace

/// @brief 创建 TCP 连接。
///
/// @param id 连接标识。
/// @param fd 已建立连接的套接字描述符（连接接管该描述符）。
/// @param peer 对端地址。
TcpConnection::TcpConnection(ConnectionId id, int fd, const sockaddr_in& peer)
    : id_(id), fd_(fd), closed_(false)
{
    char buf[INET_ADDRSTRLEN] = {0};
    const char* ip = ::inet_ntop(AF_INET, &peer.sin_addr, buf, sizeof(buf));
    if (ip != nullptr)
    {
        peerAddress_ = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));
    }
}

/// @brief 销毁连接。
TcpConnection::~TcpConnection()
{
    Close();
}

/// @brief 返回连接标识。
ConnectionId TcpConnection::id() const
{
    return id_;
}

/// @brief 返回文件描述符。
int TcpConnection::fd() const
{
    return fd_;
}

/// @brief 返回对端地址字符串。
const std::string& TcpConnection::PeerAddress() const
{
    return peerAddress_;
}

/// @brief 从 socket 读取数据到输入缓冲。
///
/// @return >0 已读取字节数；0 对端关闭；-1 出错。
ssize_t TcpConnection::Read()
{
    char buf[kReadSize];
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n > 0)
    {
        inputBuffer_.Append(buf, static_cast<size_t>(n));
    }
    return n;
}

/// @brief 返回输入缓冲可读字节数。
size_t TcpConnection::InputReadable() const
{
    return inputBuffer_.Readable();
}

/// @brief 返回输入缓冲可读区起始指针。
const char* TcpConnection::InputPeek() const
{
    return inputBuffer_.Peek();
}

/// @brief 消费输入缓冲前 len 字节。
void TcpConnection::RetrieveInput(size_t len)
{
    inputBuffer_.Retrieve(len);
}

/// @brief 发送数据。
///
/// 优先直接发送；未能全部发送的数据追加到输出缓冲，等待 POLLOUT 事件。
///
/// @return 发送的字节数（含进入输出缓冲的部分）；失败返回 -1。
ssize_t TcpConnection::Send(const char* data, size_t len)
{
    if (closed_ || fd_ < 0)
    {
        return -1;
    }
    if (outputBuffer_.Readable() > 0)
    {
        // 已有待发送数据，追加到输出缓冲
        outputBuffer_.Append(data, len);
        return static_cast<ssize_t>(len);
    }
    ssize_t n = ::send(fd_, data, len, MSG_NOSIGNAL);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // 全部进入输出缓冲
            outputBuffer_.Append(data, len);
            return static_cast<ssize_t>(len);
        }
        return -1;
    }
    if (static_cast<size_t>(n) < len)
    {
        outputBuffer_.Append(data + n, len - static_cast<size_t>(n));
    }
    return n;
}

/// @brief 刷新输出缓冲。
///
/// @return 0 成功（或 EAGAIN）；-1 出错。
ssize_t TcpConnection::Flush()
{
    while (outputBuffer_.Readable() > 0)
    {
        ssize_t n = ::send(fd_, outputBuffer_.Peek(), outputBuffer_.Readable(), MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return 0;
            }
            return -1;
        }
        outputBuffer_.Retrieve(static_cast<size_t>(n));
    }
    return 0;
}

/// @brief 输出缓冲是否有待发送数据。
bool TcpConnection::HasPendingOutput() const
{
    return outputBuffer_.Readable() > 0;
}

/// @brief 关闭连接。
void TcpConnection::Close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
    closed_ = true;
}

/// @brief 是否已关闭。
bool TcpConnection::IsClosed() const
{
    return closed_;
}

} // namespace sc
