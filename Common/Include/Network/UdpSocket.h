#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "asio.hpp"

namespace common {

/// @brief UDP Socket（基于 asio）。
///
/// 提供 UDP 数据报收发能力：绑定本地端口接收，向指定地址发送。
/// 事件循环在独立线程运行，通过回调上报数据到达事件。
class UdpSocket
{
public:
    // 数据回调：data/len 为本次数据报内容，from 为来源地址。
    using DataCallback = std::function<void(const char* data, size_t len,
                                            const std::string& from)>;

    UdpSocket();

    ~UdpSocket();

    // 绑定本地端口并开始接收（port=0 表示由系统分配）。
    bool Bind(uint16_t port, const DataCallback& dataCb);

    // 向指定地址发送数据报（线程安全）。
    bool SendTo(const std::string& host, uint16_t port, const char* data, size_t len);

    // 本地绑定端口（未绑定时返回 0）。
    uint16_t LocalPort() const;

    // 停止并等待事件循环线程退出。
    void Stop();

    // 是否正在运行。
    bool IsRunning() const;

private:
    // 事件循环线程入口。
    void ThreadMain();

    // 发起一次异步接收。
    void StartReceive();

    // 处理接收完成。
    void HandleReceive(const asio::error_code& ec, size_t bytes);

    asio::io_context io_;
    asio::ip::udp::socket socket_;
    asio::ip::udp::resolver resolver_;
    DataCallback dataCb_;
    std::thread thread_;
    std::vector<char> recvBuffer_;
    asio::ip::udp::endpoint remoteEndpoint_;
    uint16_t port_;
    std::atomic<bool> running_;
};

} // namespace common
