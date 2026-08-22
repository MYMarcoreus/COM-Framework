#include "Network/NetworkModule.h"

#include <cstdio>
#include <string>

#include "Module/ResolveContext.h"
#include "Module/InterfaceMap.h"
#include "Network/TcpServer.h"

namespace sc {

// 接口映射表：暴露本类实现的接口（查表驱动 QueryInterface）。
SC_DEFINE_INTERFACE_MAP(CNetworkModule, CModule, INetwork)

/// @brief 创建网络模块。
CNetworkModule::CNetworkModule() : CModule("network"), m_nPort(0)
{
}

/// @brief 销毁网络模块。
CNetworkModule::~CNetworkModule()
{
    Stop();
}

/// @brief 从初始化上下文解析可选的 IMetrics。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true（指标缺失不视为失败，仅不上报）。
bool CNetworkModule::Initialize(const CResolveContext& ctx)
{
    m_pMetrics.Reset(ctx.Resolve<IMetrics>());
    return true;
}

/// @brief 模块启动（服务器由 StartTcpServer 显式启动，此处无独立动作）。
bool CNetworkModule::Start()
{
    return true;
}

/// @brief 启动 TCP 服务器。
///
/// @param port 监听端口。
/// @param handler 网络事件处理器。
///
/// @return 成功返回 true。
bool CNetworkModule::StartTcpServer(uint16_t nPort, INetworkHandler* pHandler)
{
    if (pHandler == nullptr)
    {
        return false;
    }
    // 若已有运行中的服务器，先停止
    Stop();

    // 持有 pHandler 引用，确保网络运行期间 handler 存活
    m_pHandler.Reset(pHandler);

    // 将模块接口回调适配为 Common CTcpServer 的 std::function 回调
    std::unique_ptr<common::CTcpServer> pNewServer(new common::CTcpServer());
    common::CTcpServer::AcceptCallback fnAccept =
        [this](common::ConnectionId nId, const std::string& strPeer)
        {
            if (m_pMetrics != nullptr)
            {
                m_pMetrics->Inc("network.accepted");
                m_pMetrics->SetGauge("network.conns",
                    static_cast<double>(m_pServer != nullptr ? m_pServer->ConnectionCount() : 0));
            }
            if (m_pHandler != nullptr)
            {
                m_pHandler->OnAccept(nId, strPeer);
            }
        };
    common::CTcpServer::DataCallback fnData =
        [this](common::ConnectionId nId, const char* pData, size_t nLen)
        {
            if (m_pMetrics != nullptr)
            {
                m_pMetrics->Inc("network.msgs");
            }
            if (m_pHandler != nullptr)
            {
                m_pHandler->OnData(nId, pData, nLen);
            }
        };
    common::CTcpServer::CloseCallback fnClose =
        [this](common::ConnectionId nId)
        {
            if (m_pMetrics != nullptr)
            {
                m_pMetrics->Inc("network.closed");
                m_pMetrics->SetGauge("network.conns",
                    static_cast<double>(m_pServer != nullptr ? m_pServer->ConnectionCount() : 0));
            }
            if (m_pHandler != nullptr)
            {
                m_pHandler->OnClose(nId);
            }
        };
    if (!pNewServer->Start(nPort, fnAccept, fnData, fnClose))
    {
        m_pHandler.Reset();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pServer.swap(pNewServer);
        m_nPort = nPort;
    }
    return true;
}

/// @brief 停止服务器。
///
/// 先释放 server 所有权，再在锁外停止，避免等待事件循环线程时死锁。
void CNetworkModule::Stop()
{
    common::CTcpServer* pServer = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pServer = m_pServer.release();
        m_nPort = 0;
    }
    if (pServer != nullptr)
    {
        pServer->Stop(); // 等待事件循环线程退出（不持有本模块锁）
        delete pServer;
    }
    m_pHandler.Reset();
}

/// @brief 模块关闭：停止服务器。
void CNetworkModule::Shutdown()
{
    Stop();
}

/// @brief 向指定连接发送数据。
bool CNetworkModule::Send(ConnectionId nId, const char* pData, size_t nLen)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pServer == nullptr)
    {
        return false;
    }
    return m_pServer->Send(nId, pData, nLen);
}

/// @brief 关闭指定连接。
void CNetworkModule::Close(ConnectionId nId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pServer != nullptr)
    {
        m_pServer->Close(nId);
    }
}

/// @brief 挂载业务上下文。
///
/// @return 旧的上下文（无则返回 nullptr）。
void* CNetworkModule::Attach(ConnectionId nId, void* pCtx)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    void* pOld = m_mapConnCtx[nId];
    m_mapConnCtx[nId] = pCtx;
    return pOld;
}

/// @brief 取回业务上下文。
void* CNetworkModule::GetAttached(ConnectionId nId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<ConnectionId, void*>::const_iterator it = m_mapConnCtx.find(nId);
    return (it != m_mapConnCtx.end()) ? it->second : nullptr;
}

/// @brief 移除并返回业务上下文（所有权交还调用方）。
void* CNetworkModule::Detach(ConnectionId nId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<ConnectionId, void*>::iterator it = m_mapConnCtx.find(nId);
    if (it == m_mapConnCtx.end())
    {
        return nullptr;
    }
    void* pCtx = it->second;
    m_mapConnCtx.erase(it);
    return pCtx;
}

/// @brief 返回当前监听端口。
uint16_t CNetworkModule::ListeningPort() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nPort;
}

/// @brief 当前活跃连接数。
size_t CNetworkModule::ConnectionCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_pServer != nullptr) ? m_pServer->ConnectionCount() : 0;
}

/// @brief 累计接受连接数。
uint64_t CNetworkModule::TotalAccepted() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_pServer != nullptr) ? m_pServer->TotalAccepted() : 0;
}

/// @brief 累计关闭连接数。
uint64_t CNetworkModule::TotalClosed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_pServer != nullptr) ? m_pServer->TotalClosed() : 0;
}

/// @brief 指定连接是否存在。
bool CNetworkModule::HasConnection(ConnectionId nId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pServer != nullptr && m_pServer->HasConnection(nId);
}

/// @brief 指定连接的对端地址。
std::string CNetworkModule::PeerAddress(ConnectionId nId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_pServer != nullptr) ? m_pServer->PeerAddress(nId) : "";
}

/// @brief 设置空闲超时。
///
/// @param nSeconds 空闲秒数；0 表示禁用。
void CNetworkModule::SetIdleTimeout(uint32_t nSeconds)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pServer != nullptr)
    {
        m_pServer->SetIdleTimeout(nSeconds);
    }
}

/// @brief 设置最大连接数上限。
///
/// @param nMax 连接数上限；0 表示不限制。
void CNetworkModule::SetMaxConnections(size_t nMax)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pServer != nullptr)
    {
        m_pServer->SetMaxConnections(nMax);
    }
}

/// @brief 网络模块状态报告。
///
/// @return 形如 "network:port=9000 conns=2 accepted=5 closed=3"。
std::string CNetworkModule::GetStatus() const
{
    char szBuffer[128];
    std::snprintf(szBuffer, sizeof(szBuffer), "network:port=%u conns=%zu accepted=%llu closed=%llu",
                  static_cast<unsigned int>(ListeningPort()), ConnectionCount(),
                  static_cast<unsigned long long>(TotalAccepted()),
                  static_cast<unsigned long long>(TotalClosed()));
    return std::string(szBuffer);
}

} // namespace sc
