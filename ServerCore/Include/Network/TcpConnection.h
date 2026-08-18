#pragma once

#include <cstddef>
#include <netinet/in.h>
#include <string>
#include <sys/types.h>

#include "Common/Types.h"
#include "Network/Buffer.h"

namespace sc {

/// @brief TCP 连接。
///
/// 封装一个已建立的连接，包含输入/输出缓冲与对端地址。
/// 所有读写均为非阻塞方式，由事件循环驱动。
class TcpConnection
{
public:
    // 创建连接。
    TcpConnection(ConnectionId id, int fd, const sockaddr_in& peer);

    ~TcpConnection();

    // 连接标识。
    ConnectionId id() const;

    // 文件描述符。
    int fd() const;

    // 对端地址字符串。
    const std::string& PeerAddress() const;

    // 从 socket 读取数据到输入缓冲。
    // 返回 >0 已读取字节数；0 对端关闭；-1 出错。
    ssize_t Read();

    // 输入缓冲可读字节数。
    size_t InputReadable() const;

    // 输入缓冲可读区起始指针。
    const char* InputPeek() const;

    // 消费输入缓冲前 len 字节。
    void RetrieveInput(size_t len);

    // 发送数据，未发送完的数据进入输出缓冲。
    ssize_t Send(const char* data, size_t len);

    // 刷新输出缓冲（非阻塞发送）。
    ssize_t Flush();

    // 输出缓冲是否有待发送数据。
    bool HasPendingOutput() const;

    // 关闭连接。
    void Close();

    // 是否已关闭。
    bool IsClosed() const;

private:
    ConnectionId id_;
    int fd_;
    std::string peerAddress_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;
    bool closed_;
};

} // namespace sc
