#include "Network/TcpServer.h"

#include <functional>
#include <vector>

namespace common {

/// @brief 创建 TCP 服务器。
TcpServer::TcpServer()
    : nextId_(1), port_(0), running_(false)
{
}

/// @brief 销毁 TCP 服务器。
TcpServer::~TcpServer()
{
    Stop();
}

/// @brief 启动服务器并监听端口。
///
/// @param port 监听端口。
/// @param acceptCb 新连接回调。
/// @param dataCb 数据回调。
/// @param closeCb 连接关闭回调。
///
/// @return 成功返回 true。
bool TcpServer::Start(uint16_t port, const AcceptCallback& acceptCb,
                      const DataCallback& dataCb, const CloseCallback& closeCb)
{
    if (running_.load())
    {
        return false;
    }
    acceptCb_ = acceptCb;
    dataCb_ = dataCb;
    closeCb_ = closeCb;

    // ① 创建并配置 acceptor
    asio::error_code ec;
    acceptor_.reset(new asio::ip::tcp::acceptor(io_));
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), port);
    static_cast<void>(acceptor_->open(endpoint.protocol(), ec));
    if (!ec)
    {
        static_cast<void>(acceptor_->set_option(asio::ip::tcp::acceptor::reuse_address(true), ec));
    }
    if (!ec)
    {
        static_cast<void>(acceptor_->bind(endpoint, ec));
    }
    if (!ec)
    {
        static_cast<void>(acceptor_->listen(asio::socket_base::max_listen_connections, ec));
    }
    if (ec)
    {
        return false;
    }

    port_ = port;
    running_.store(true);
    thread_ = std::thread(&TcpServer::ThreadMain, this);
    return true;
}

/// @brief 停止服务器。
///
/// 投递关闭任务到事件循环线程，等待线程退出。
void TcpServer::Stop()
{
    if (!running_.load())
    {
        return;
    }
    running_.store(false);
    asio::post(io_, [this]() { ShutdownOnIoThread(); });
    if (thread_.joinable())
    {
        thread_.join();
    }
}

/// @brief 事件循环线程入口。
void TcpServer::ThreadMain()
{
    StartAccept();
    io_.run();
    // 事件循环退出后兜底关闭剩余连接
    ShutdownOnIoThread();
}

/// @brief 发起一次异步 accept。
void TcpServer::StartAccept()
{
    if (acceptor_ == nullptr || !acceptor_->is_open())
    {
        return;
    }
    acceptor_->async_accept(
        [this](const asio::error_code& ec, asio::ip::tcp::socket socket)
        {
            // ① 错误处理
            if (ec)
            {
                if (ec == asio::error::operation_aborted)
                {
                    return; // 服务器关闭中
                }
                if (acceptor_ != nullptr && acceptor_->is_open())
                {
                    StartAccept(); // 瞬时错误，继续接受
                }
                return;
            }
            // ② 创建连接并注册
            ConnectionId id = nextId_++;
            TcpConnection::Ptr conn =
                std::make_shared<TcpConnection>(io_, id, std::move(socket));
            conn->SetCallbacks(
                std::bind(&TcpServer::HandleData, this, std::placeholders::_1,
                          std::placeholders::_2, std::placeholders::_3),
                std::bind(&TcpServer::HandleClose, this, std::placeholders::_1));
            connections_[id] = conn;
            conn->StartRead();
            if (acceptCb_)
            {
                acceptCb_(id, conn->PeerAddress());
            }
            StartAccept(); // 继续接受下一个连接
        });
}

/// @brief 连接数据回调。
void TcpServer::HandleData(const TcpConnection::Ptr& conn, const char* data, size_t len)
{
    if (dataCb_)
    {
        dataCb_(conn->id(), data, len);
    }
}

/// @brief 连接关闭回调。
///
/// 从连接管理表中移除连接，并通知上层。
void TcpServer::HandleClose(const TcpConnection::Ptr& conn)
{
    ConnectionId id = conn->id();
    connections_.erase(id);
    if (closeCb_)
    {
        closeCb_(id);
    }
}

/// @brief 向指定连接发送数据。
///
/// 查找与投递均在事件循环线程执行，线程安全。
bool TcpServer::Send(ConnectionId id, const char* data, size_t len)
{
    if (!running_.load())
    {
        return false;
    }
    std::string payload(data, len);
    asio::post(io_, [this, id, payload]()
    {
        std::map<ConnectionId, TcpConnection::Ptr>::iterator it = connections_.find(id);
        if (it != connections_.end())
        {
            it->second->Send(payload.data(), payload.size());
        }
    });
    return true;
}

/// @brief 关闭指定连接。
void TcpServer::Close(ConnectionId id)
{
    asio::post(io_, [this, id]()
    {
        std::map<ConnectionId, TcpConnection::Ptr>::iterator it = connections_.find(id);
        if (it == connections_.end())
        {
            return;
        }
        TcpConnection::Ptr conn = it->second;
        connections_.erase(it);
        conn->Close();
        if (closeCb_)
        {
            closeCb_(id);
        }
    });
}

/// @brief 在 io 线程内执行关闭流程。
///
/// 关闭 acceptor 与所有连接，幂等可重复调用。
void TcpServer::ShutdownOnIoThread()
{
    if (acceptor_ != nullptr && acceptor_->is_open())
    {
        asio::error_code ignore;
        static_cast<void>(acceptor_->close(ignore));
    }
    std::vector<TcpConnection::Ptr> all;
    for (std::map<ConnectionId, TcpConnection::Ptr>::iterator it = connections_.begin();
         it != connections_.end(); ++it)
    {
        all.push_back(it->second);
    }
    connections_.clear();
    for (size_t i = 0; i < all.size(); ++i)
    {
        all[i]->Close();
        if (closeCb_)
        {
            closeCb_(all[i]->id());
        }
    }
}

/// @brief 返回当前监听端口。
uint16_t TcpServer::ListeningPort() const
{
    return port_;
}

/// @brief 是否正在运行。
bool TcpServer::IsRunning() const
{
    return running_.load();
}

} // namespace common
