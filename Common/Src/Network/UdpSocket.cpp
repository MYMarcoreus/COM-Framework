#include "Network/UdpSocket.h"

#include <functional>

namespace common {

/// @brief 创建 UDP Socket。
UdpSocket::UdpSocket()
    : socket_(io_), resolver_(io_), port_(0), running_(false)
{
    recvBuffer_.resize(65536);
}

/// @brief 销毁 UDP Socket。
UdpSocket::~UdpSocket()
{
    Stop();
}

/// @brief 绑定本地端口并开始接收。
///
/// @param port 本地端口；0 表示由系统分配。
/// @param dataCb 数据到达回调。
///
/// @return 绑定成功返回 true。
bool UdpSocket::Bind(uint16_t port, const DataCallback& dataCb)
{
    if (running_.load())
    {
        return false;
    }
    dataCb_ = dataCb;
    asio::error_code ec;
    static_cast<void>(socket_.open(asio::ip::udp::v4(), ec));
    if (ec)
    {
        return false;
    }
    static_cast<void>(socket_.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), port), ec));
    if (ec)
    {
        return false;
    }
    asio::ip::udp::endpoint local = socket_.local_endpoint(ec);
    if (ec)
    {
        return false;
    }
    port_ = local.port();
    running_.store(true);
    thread_ = std::thread(&UdpSocket::ThreadMain, this);
    return true;
}

/// @brief 向指定地址发送数据报。
///
/// 解析目标地址后投递到事件循环线程发送，线程安全。
///
/// @return 已运行且参数合法时返回 true。
bool UdpSocket::SendTo(const std::string& host, uint16_t port, const char* data, size_t len)
{
    if (!running_.load() || data == nullptr || len == 0)
    {
        return false;
    }
    std::string payload(data, len);
    resolver_.async_resolve(host, std::to_string(port),
        [this, payload](const asio::error_code& ec,
                        asio::ip::udp::resolver::results_type results)
        {
            if (ec || results.empty())
            {
                return;
            }
            asio::ip::udp::endpoint target = *results.begin();
            asio::post(io_,
                [this, payload, target]()
                {
                    if (!socket_.is_open())
                    {
                        return;
                    }
                    asio::error_code ignored;
                    static_cast<void>(socket_.send_to(asio::buffer(payload), target, 0, ignored));
                });
        });
    return true;
}

/// @brief 本地绑定端口。
uint16_t UdpSocket::LocalPort() const
{
    return port_;
}

/// @brief 停止并等待事件循环线程退出。
void UdpSocket::Stop()
{
    if (!running_.load())
    {
        return;
    }
    running_.store(false);
    asio::error_code ignored;
    static_cast<void>(socket_.cancel(ignored));
    static_cast<void>(socket_.close(ignored));
    if (thread_.joinable())
    {
        thread_.join();
    }
}

/// @brief 是否正在运行。
bool UdpSocket::IsRunning() const
{
    return running_.load();
}

/// @brief 事件循环线程入口。
void UdpSocket::ThreadMain()
{
    StartReceive();
    io_.run();
    running_.store(false);
}

/// @brief 发起一次异步接收。
void UdpSocket::StartReceive()
{
    if (!socket_.is_open())
    {
        return;
    }
    socket_.async_receive_from(asio::buffer(recvBuffer_), remoteEndpoint_,
        [this](const asio::error_code& ec, size_t bytes)
        {
            HandleReceive(ec, bytes);
        });
}

/// @brief 处理接收完成。
void UdpSocket::HandleReceive(const asio::error_code& ec, size_t bytes)
{
    if (ec)
    {
        if (ec != asio::error::operation_aborted && socket_.is_open())
        {
            StartReceive(); // 瞬时错误，继续接收
        }
        return;
    }
    if (dataCb_)
    {
        std::string from = remoteEndpoint_.address().to_string() + ":" +
                           std::to_string(remoteEndpoint_.port());
        dataCb_(recvBuffer_.data(), bytes, from);
    }
    if (running_.load() && socket_.is_open())
    {
        StartReceive();
    }
}

} // namespace common
