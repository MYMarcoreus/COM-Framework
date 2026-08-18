#include "Network/udp_socket.h"

#include <functional>

namespace common {

/// @brief 创建 UDP Socket。
CUdpSocket::CUdpSocket()
    : m_socket(m_io), m_resolver(m_io), m_nPort(0), m_bRunning(false)
{
    m_vecRecvBuffer.resize(65536);
}

/// @brief 销毁 UDP Socket。
CUdpSocket::~CUdpSocket()
{
    Stop();
}

/// @brief 绑定本地端口并开始接收。
///
/// @param port 本地端口；0 表示由系统分配。
/// @param dataCb 数据到达回调。
///
/// @return 绑定成功返回 true。
bool CUdpSocket::Bind(uint16_t port, const DataCallback& dataCb)
{
    if (m_bRunning.load())
    {
        return false;
    }
    m_fnData = dataCb;
    asio::error_code ec;
    static_cast<void>(m_socket.open(asio::ip::udp::v4(), ec));
    if (ec)
    {
        return false;
    }
    static_cast<void>(m_socket.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), port), ec));
    if (ec)
    {
        return false;
    }
    asio::ip::udp::endpoint local = m_socket.local_endpoint(ec);
    if (ec)
    {
        return false;
    }
    m_nPort = local.port();
    m_bRunning.store(true);
    m_thread = std::thread(&CUdpSocket::ThreadMain, this);
    return true;
}

/// @brief 向指定地址发送数据报。
///
/// 解析目标地址后投递到事件循环线程发送，线程安全。
///
/// @return 已运行且参数合法时返回 true。
bool CUdpSocket::SendTo(const std::string& host, uint16_t port, const char* data, size_t len)
{
    if (!m_bRunning.load() || data == nullptr || len == 0)
    {
        return false;
    }
    std::string payload(data, len);
    m_resolver.async_resolve(host, std::to_string(port),
        [this, payload](const asio::error_code& ec,
                        asio::ip::udp::resolver::results_type results)
        {
            if (ec || results.empty())
            {
                return;
            }
            asio::ip::udp::endpoint target = *results.begin();
            asio::post(m_io,
                [this, payload, target]()
                {
                    if (!m_socket.is_open())
                    {
                        return;
                    }
                    asio::error_code ignored;
                    static_cast<void>(m_socket.send_to(asio::buffer(payload), target, 0, ignored));
                });
        });
    return true;
}

/// @brief 本地绑定端口。
uint16_t CUdpSocket::LocalPort() const
{
    return m_nPort;
}

/// @brief 停止并等待事件循环线程退出。
void CUdpSocket::Stop()
{
    if (!m_bRunning.load())
    {
        return;
    }
    m_bRunning.store(false);
    asio::error_code ignored;
    static_cast<void>(m_socket.cancel(ignored));
    static_cast<void>(m_socket.close(ignored));
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

/// @brief 是否正在运行。
bool CUdpSocket::IsRunning() const
{
    return m_bRunning.load();
}

/// @brief 事件循环线程入口。
void CUdpSocket::ThreadMain()
{
    StartReceive();
    m_io.run();
    m_bRunning.store(false);
}

/// @brief 发起一次异步接收。
void CUdpSocket::StartReceive()
{
    if (!m_socket.is_open())
    {
        return;
    }
    m_socket.async_receive_from(asio::buffer(m_vecRecvBuffer), m_remoteEndpoint,
        [this](const asio::error_code& ec, size_t bytes)
        {
            HandleReceive(ec, bytes);
        });
}

/// @brief 处理接收完成。
void CUdpSocket::HandleReceive(const asio::error_code& ec, size_t bytes)
{
    if (ec)
    {
        if (ec != asio::error::operation_aborted && m_socket.is_open())
        {
            StartReceive(); // 瞬时错误，继续接收
        }
        return;
    }
    if (m_fnData)
    {
        std::string from = m_remoteEndpoint.address().to_string() + ":" +
                           std::to_string(m_remoteEndpoint.port());
        m_fnData(m_vecRecvBuffer.data(), bytes, from);
    }
    if (m_bRunning.load() && m_socket.is_open())
    {
        StartReceive();
    }
}

} // namespace common
