#pragma once

#include <cstdint>
#include <netinet/in.h>

#include "Network/Socket.h"

namespace sc {

/// @brief 连接接受器。
///
/// 封装监听套接字，负责 bind / listen / accept。
class Acceptor
{
public:
    Acceptor();

    ~Acceptor();

    // 创建监听套接字并绑定、监听指定端口。
    bool BindAndListen(uint16_t port, int backlog = 64);

    // 监听套接字描述符。
    int fd() const;

    // 接受新连接，返回新连接描述符。
    int Accept(sockaddr_in* peer);

    // 是否有效。
    bool IsValid() const;

private:
    Socket listenSocket_;
};

} // namespace sc
