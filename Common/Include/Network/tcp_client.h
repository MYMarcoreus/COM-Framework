#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "asio.hpp"

#include "Network/buffer.h"

namespace common {

/// @brief TCP 客户端（基于 asio）。
///
/// 主动连接远程服务器，异步读写，事件循环在独立线程运行。
/// 通过回调上报连接结果 / 数据 / 关闭事件，不依赖具体业务接口。
class CTcpClient
{
public:
    // 连接结果回调：success 表示是否连接成功，peer 为对端地址。
    using ConnectCallback = std::function<void(bool success, const std::string& peer)>;

    // 数据回调。
    using DataCallback = std::function<void(const char* data, size_t len)>;

    // 连接关闭回调（对端关闭或本地关闭）。
    using CloseCallback = std::function<void()>;

    CTcpClient();

    ~CTcpClient();

    // 异步连接远程服务器。
    bool Connect(const std::string& host, uint16_t port, const ConnectCallback& connectCb,
                 const DataCallback& dataCb, const CloseCallback& closeCb);

    // 发送数据（线程安全）。
    bool Send(const char* data, size_t len);

    // 关闭连接（线程安全）。
    void Close();

    // 停止客户端，等待事件循环线程退出。
    void Stop();

    // 是否已连接。
    bool IsConnected() const;

    // 对端地址字符串。
    std::string PeerAddress() const;

private:
    // 事件循环线程入口。
    void ThreadMain();

    // 发起异步连接。
    void StartConnect();

    // 处理连接完成。
    void HandleConnect(const asio::error_code& ec, const asio::ip::tcp::endpoint& endpoint);

    // 发起异步读。
    void DoRead();

    // 处理读完成。
    void HandleRead(const asio::error_code& ec, size_t bytes);

    // 追加待发送数据并启动写。
    void AppendWrite(const std::string& data);

    // 发起异步写。
    void DoWrite();

    // 处理写完成。
    void HandleWrite(const asio::error_code& ec, size_t bytes);

    // 在 io 线程内关闭连接。
    void CloseOnIoThread();

    // 通知上层连接关闭（仅一次）。
    void NotifyClose();

    asio::io_context io_;
    asio::ip::tcp::socket socket_;
    asio::ip::tcp::resolver resolver_;
    ConnectCallback connectCb_;
    DataCallback dataCb_;
    CloseCallback closeCb_;
    std::string host_;
    uint16_t port_;
    std::thread thread_;
    CBuffer inputBuffer_;
    std::string pendingOutput_;
    std::vector<char> readBuffer_;
    std::string peerAddress_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::atomic<bool> closeNotified_;
};

} // namespace common
