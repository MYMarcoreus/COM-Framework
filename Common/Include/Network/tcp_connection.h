#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "asio.hpp"

#include "Network/network_types.h"
#include "Network/buffer.h"

namespace common {

/// @brief TCP 连接（基于 asio）。
///
/// 封装一个已建立的连接，基于 asio::ip::tcp::socket 提供异步读写。
/// 生命周期由 shared_ptr 管理（供异步回调安全持有）。
/// 所有读写回调都在事件循环线程执行；Send/Close 通过投递保证线程安全。
class CTcpConnection : public std::enable_shared_from_this<CTcpConnection>
{
public:
    /// @brief 连接指针。
    using Ptr = std::shared_ptr<CTcpConnection>;

    /// @brief 数据回调。
    using DataCallback = std::function<void(const Ptr& conn, const char* data, size_t len)>;

    /// @brief 连接关闭回调。
    using CloseCallback = std::function<void(const Ptr& conn)>;

    // 创建连接。
    CTcpConnection(asio::io_context& io, ConnectionId id, asio::ip::tcp::socket socket);

    ~CTcpConnection();

    // 连接标识。
    ConnectionId id() const;

    // 对端地址字符串。
    const std::string& PeerAddress() const;

    // 设置数据与关闭回调。
    void SetCallbacks(const DataCallback& dataCallback, const CloseCallback& closeCallback);

    // 开始异步读取。
    void StartRead();

    // 发送数据（线程安全）。
    void Send(const char* data, size_t len);

    // 关闭连接（线程安全）。
    void Close();

    // 是否已关闭。
    bool IsClosed() const;

private:
    // 发起一次异步读。
    void DoRead();

    // 处理读完成。
    void HandleRead(const asio::error_code& ec, size_t bytes);

    // 追加待发送数据并启动写。
    void AppendWrite(const std::string& data);

    // 发起一次异步写。
    void DoWrite();

    // 处理写完成。
    void HandleWrite(const asio::error_code& ec, size_t bytes);

    // 在 io 线程内关闭连接。
    void CloseOnIoThread();

    // 处理错误或对端关闭。
    void HandleError(const asio::error_code& ec);

    asio::io_context& io_;
    ConnectionId id_;
    asio::ip::tcp::socket socket_;
    std::string peerAddress_;
    CBuffer inputBuffer_;
    std::string pendingOutput_;
    std::vector<char> readBuffer_;
    DataCallback dataCallback_;
    CloseCallback closeCallback_;
    std::atomic<bool> writing_;
    std::atomic<bool> closed_;
};

} // namespace common
