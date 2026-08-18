#include "Network/Acceptor.h"

#include <sys/socket.h>

namespace sc {

/// @brief 创建连接接受器。
Acceptor::Acceptor()
{
}

/// @brief 销毁连接接受器。
Acceptor::~Acceptor()
{
}

/// @brief 创建监听套接字并绑定、监听端口。
///
/// @param port 监听端口。
/// @param backlog 监听队列长度。
///
/// @return 成功返回 true。
bool Acceptor::BindAndListen(uint16_t port, int backlog)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return false;
    }
    listenSocket_ = Socket(fd);
    return listenSocket_.BindAndListen(port, backlog);
}

/// @brief 返回监听套接字描述符。
int Acceptor::fd() const
{
    return listenSocket_.fd();
}

/// @brief 接受新连接。
///
/// @param peer 输出对端地址。
///
/// @return 新连接描述符；失败返回 -1。
int Acceptor::Accept(sockaddr_in* peer)
{
    return listenSocket_.Accept(peer);
}

/// @brief 是否有效。
bool Acceptor::IsValid() const
{
    return listenSocket_.IsValid();
}

} // namespace sc
