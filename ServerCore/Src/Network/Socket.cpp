#include "Network/Socket.h"

#include <cstring>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sc {

/// @brief 创建空 Socket。
Socket::Socket() : fd_(-1)
{
}

/// @brief 接管指定文件描述符。
///
/// @param fd 已创建的文件描述符。
Socket::Socket(int fd) : fd_(fd)
{
}

/// @brief 移动构造。
Socket::Socket(Socket&& other) noexcept : fd_(other.fd_)
{
    other.fd_ = -1;
}

/// @brief 移动赋值。
Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other)
    {
        Close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

/// @brief 析构，关闭描述符。
Socket::~Socket()
{
    Close();
}

/// @brief 返回文件描述符。
int Socket::fd() const
{
    return fd_;
}

/// @brief 是否持有有效描述符。
bool Socket::IsValid() const
{
    return fd_ >= 0;
}

/// @brief 设置为非阻塞。
void Socket::SetNonBlocking()
{
    if (fd_ < 0)
    {
        return;
    }
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0)
    {
        return;
    }
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
}

/// @brief 设置 SO_REUSEADDR。
void Socket::SetReuseAddr()
{
    if (fd_ < 0)
    {
        return;
    }
    int optval = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

/// @brief 设置 TCP_NODELAY。
void Socket::SetNoDelay()
{
    if (fd_ < 0)
    {
        return;
    }
    int optval = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
}

/// @brief 绑定并监听指定端口。
///
/// @param port 监听端口。
/// @param backlog 监听队列长度。
///
/// @return 成功返回 true。
bool Socket::BindAndListen(uint16_t port, int backlog)
{
    if (fd_ < 0)
    {
        return false;
    }
    SetReuseAddr();
    SetNonBlocking();

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        return false;
    }
    if (::listen(fd_, backlog) < 0)
    {
        return false;
    }
    return true;
}

/// @brief 接受新连接。
///
/// @param peer 输出对端地址。
///
/// @return 新连接描述符；失败返回 -1。
int Socket::Accept(sockaddr_in* peer)
{
    if (fd_ < 0)
    {
        return -1;
    }
    socklen_t len = sizeof(sockaddr_in);
    return ::accept(fd_, reinterpret_cast<sockaddr*>(peer), &len);
}

/// @brief 释放描述符所有权。
///
/// @return 描述符，调用后本对象不再持有该描述符。
int Socket::Release()
{
    int fd = fd_;
    fd_ = -1;
    return fd;
}

/// @brief 关闭描述符。
void Socket::Close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace sc
