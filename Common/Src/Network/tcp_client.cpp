#include "Network/tcp_client.h"

#include <functional>

namespace common {

/// @brief 创建 TCP 客户端。
CTcpClient::CTcpClient()
    : socket_(io_), resolver_(io_), port_(0), running_(false),
      connected_(false), closeNotified_(false)
{
    readBuffer_.resize(8192);
}

/// @brief 销毁 TCP 客户端。
CTcpClient::~CTcpClient()
{
    Stop();
}

/// @brief 异步连接远程服务器。
///
/// @param host 远程主机名或 IP。
/// @param port 远程端口。
/// @param connectCb 连接结果回调。
/// @param dataCb 数据回调。
/// @param closeCb 连接关闭回调。
///
/// @return 成功发起连接返回 true。
bool CTcpClient::Connect(const std::string& host, uint16_t port,
                        const ConnectCallback& connectCb, const DataCallback& dataCb,
                        const CloseCallback& closeCb)
{
    if (running_.load())
    {
        return false; // 已在连接中
    }
    host_ = host;
    port_ = port;
    connectCb_ = connectCb;
    dataCb_ = dataCb;
    closeCb_ = closeCb;
    connected_.store(false);
    closeNotified_.store(false);
    running_.store(true);
    thread_ = std::thread(&CTcpClient::ThreadMain, this);
    return true;
}

/// @brief 发送数据。
///
/// 投递到事件循环线程写入，线程安全。
///
/// @return 已连接时返回 true。
bool CTcpClient::Send(const char* data, size_t len)
{
    if (!connected_.load() || data == nullptr || len == 0)
    {
        return false;
    }
    std::string payload(data, len);
    asio::post(io_, [this, payload]() { AppendWrite(payload); });
    return true;
}

/// @brief 关闭连接。
///
/// 投递到事件循环线程关闭，线程安全。
void CTcpClient::Close()
{
    if (!running_.load())
    {
        return;
    }
    asio::post(io_, [this]() { CloseOnIoThread(); });
}

/// @brief 停止客户端。
///
/// 投递关闭任务到事件循环线程并等待线程退出。
/// 若线程因连接失败/关闭已自然退出，仍执行 join 避免未 join 线程析构。
void CTcpClient::Stop()
{
    if (running_.load())
    {
        running_.store(false);
        asio::post(io_, [this]() { CloseOnIoThread(); });
    }
    if (thread_.joinable())
    {
        thread_.join();
    }
}

/// @brief 是否已连接。
bool CTcpClient::IsConnected() const
{
    return connected_.load();
}

/// @brief 对端地址字符串。
std::string CTcpClient::PeerAddress() const
{
    return peerAddress_;
}

/// @brief 事件循环线程入口。
void CTcpClient::ThreadMain()
{
    StartConnect();
    io_.run();
    running_.store(false);
}

/// @brief 发起异步连接。
///
/// 先解析主机名，再异步连接。
void CTcpClient::StartConnect()
{
    resolver_.async_resolve(host_, std::to_string(port_),
        [this](const asio::error_code& ec, asio::ip::tcp::resolver::results_type results)
        {
            if (ec)
            {
                // 解析失败：通知连接失败
                if (connectCb_)
                {
                    connectCb_(false, "");
                }
                NotifyClose();
                return;
            }
            asio::async_connect(socket_, results,
                [this](const asio::error_code& cerr,
                       const asio::ip::tcp::endpoint& endpoint)
                {
                    HandleConnect(cerr, endpoint);
                });
        });
}

/// @brief 处理连接完成。
void CTcpClient::HandleConnect(const asio::error_code& ec, const asio::ip::tcp::endpoint& endpoint)
{
    if (ec)
    {
        if (connectCb_)
        {
            connectCb_(false, "");
        }
        NotifyClose();
        return;
    }
    peerAddress_ = endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    connected_.store(true);
    if (connectCb_)
    {
        connectCb_(true, peerAddress_);
    }
    DoRead();
}

/// @brief 发起一次异步读。
void CTcpClient::DoRead()
{
    socket_.async_read_some(asio::buffer(readBuffer_),
        [this](const asio::error_code& ec, size_t bytes)
        {
            HandleRead(ec, bytes);
        });
}

/// @brief 处理读完成。
void CTcpClient::HandleRead(const asio::error_code& ec, size_t bytes)
{
    if (ec)
    {
        if (ec != asio::error::operation_aborted)
        {
            NotifyClose();
        }
        return;
    }
    inputBuffer_.Append(readBuffer_.data(), bytes);
    if (dataCb_)
    {
        dataCb_(inputBuffer_.Peek(), inputBuffer_.Readable());
    }
    inputBuffer_.RetrieveAll();
    if (!connected_.load())
    {
        return;
    }
    DoRead();
}

/// @brief 追加待发送数据并启动写。
void CTcpClient::AppendWrite(const std::string& data)
{
    if (!socket_.is_open())
    {
        return;
    }
    bool writing = !pendingOutput_.empty();
    pendingOutput_.append(data);
    if (!writing)
    {
        DoWrite();
    }
}

/// @brief 发起一次异步写。
void CTcpClient::DoWrite()
{
    socket_.async_write_some(asio::buffer(pendingOutput_),
        [this](const asio::error_code& ec, size_t bytes)
        {
            HandleWrite(ec, bytes);
        });
}

/// @brief 处理写完成。
void CTcpClient::HandleWrite(const asio::error_code& ec, size_t bytes)
{
    if (ec)
    {
        if (ec != asio::error::operation_aborted)
        {
            NotifyClose();
        }
        return;
    }
    pendingOutput_.erase(0, bytes);
    if (!pendingOutput_.empty())
    {
        DoWrite();
    }
}

/// @brief 在 io 线程内关闭连接。
void CTcpClient::CloseOnIoThread()
{
    if (socket_.is_open())
    {
        asio::error_code ignored;
        static_cast<void>(socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored));
        static_cast<void>(socket_.close(ignored));
    }
    connected_.store(false);
}

/// @brief 通知上层连接关闭（仅一次）。
void CTcpClient::NotifyClose()
{
    bool expected = false;
    if (closeNotified_.compare_exchange_strong(expected, true))
    {
        if (closeCb_)
        {
            closeCb_();
        }
    }
}

} // namespace common
