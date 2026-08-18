#pragma once

#include <cstdint>
#include <netinet/in.h>

namespace sc {

/// @brief Socket 封装。
///
/// RAII 管理文件描述符，仅支持移动语义，不可拷贝。
class Socket
{
public:
    // 创建空 Socket。
    Socket();

    // 接管指定文件描述符。
    explicit Socket(int fd);

    // 移动构造。
    Socket(Socket&& other) noexcept;

    // 移动赋值。
    Socket& operator=(Socket&& other) noexcept;

    // 拷贝构造与拷贝赋值被禁止。
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    ~Socket();

    // 文件描述符。
    int fd() const;

    // 是否持有有效描述符。
    bool IsValid() const;

    // 设置为非阻塞。
    void SetNonBlocking();

    // 设置 SO_REUSEADDR。
    void SetReuseAddr();

    // 设置 TCP_NODELAY。
    void SetNoDelay();

    // 绑定并监听指定端口。
    bool BindAndListen(uint16_t port, int backlog = 64);

    // 接受新连接，返回新连接描述符。
    int Accept(sockaddr_in* peer);

    // 释放描述符所有权（返回描述符并置为无效）。
    int Release();

    // 关闭描述符。
    void Close();

private:
    int fd_;
};

} // namespace sc
