#include "Network/TcpServerModule.h"

#include <string>

#include "Log/Logger.h"
#include "Module/ResolveContext.h"

namespace sc {

/// @brief 创建 TCP 服务器装配模块。
///
/// @param nPort   监听端口。
/// @param strName 模块名（默认 "network"）。
CTcpServerModule::CTcpServerModule(std::uint16_t nPort, const char* strName)
    : CModule(strName), m_nPort(nPort)
{
    // 硬依赖 + 顺序约束：
    //  - INetwork：Start 时要调用 StartTcpServer，必须先启动；
    //  - INetworkHandler：作为回调对象，其 Initialize 必须先就绪
    //    （否则连接到来时 handler 内部状态可能未准备好）。
    AddDependency(IID_INetwork());
    AddDependency(IID_INetworkHandler());
}

/// @brief 销毁 TCP 服务器装配模块。
CTcpServerModule::~CTcpServerModule()
{
}

/// @brief 从初始化上下文获取网络接口并建立关联。
///
/// 获取 INetwork、INetworkHandler（协议处理服务）与 IEventDispatcher 接口。
///
/// @param ctx 初始化上下文（按类型自动绑定接口标识）。
///
/// @return true 关联成功；false 网络接口或处理接口模块缺失。
bool CTcpServerModule::Initialize(const CResolveContext& ctx)
{
    // ① 获取网络接口
    INetwork* pNetwork = ctx.Resolve<INetwork>();
    if (pNetwork == nullptr)
    {
        return false;
    }
    m_pNetwork.Reset(pNetwork);

    // ② 获取网络事件处理接口（协议处理服务）
    INetworkHandler* pHandler = ctx.Resolve<INetworkHandler>();
    if (pHandler == nullptr)
    {
        return false;
    }
    m_pHandler.Reset(pHandler);

    // ③ 获取事件分发器（可选，用于发布生命周期事件）
    m_pEventDispatcher.Reset(ctx.Resolve<IEventDispatcher>());
    return true;
}

/// @brief 启动 TCP 服务器。
///
/// 启动成功后发布 "network.started" 事件（负载为监听端口）。
///
/// @return true 启动成功；false 启动失败（如端口被占用）。
bool CTcpServerModule::Start()
{
    if (m_pNetwork == nullptr || m_pHandler == nullptr)
    {
        return false;
    }
    if (!m_pNetwork->StartTcpServer(m_nPort, m_pHandler.Get()))
    {
        return false;
    }
    if (m_pEventDispatcher != nullptr)
    {
        m_pEventDispatcher->Publish(sc::events::kNetworkStarted, &m_nPort, sizeof(m_nPort));
    }
    common::CLogger::Instance().Info("TCP 服务器已启动，监听端口 " + std::to_string(m_nPort));
    return true;
}

/// @brief 停止服务器。
///
/// 停止前发布 "network.stopped" 事件。
void CTcpServerModule::Stop()
{
    if (m_pEventDispatcher != nullptr)
    {
        m_pEventDispatcher->Publish(sc::events::kNetworkStopped, nullptr, 0);
    }
    if (m_pNetwork != nullptr)
    {
        m_pNetwork->Stop();
    }
}

/// @brief 停止服务器并释放引用。
void CTcpServerModule::Shutdown()
{
    Stop();
    m_pNetwork.Reset();
    m_pHandler.Reset();
    m_pEventDispatcher.Reset();
}

/// @brief 状态报告。
///
/// @return 形如 "tcp-server:port=9200 conns=0 accepted=2 closed=2" 的状态描述。
std::string CTcpServerModule::GetStatus() const
{
    std::string strStatus = "tcp-server:port=";
    if (m_pNetwork != nullptr)
    {
        strStatus += std::to_string(m_pNetwork->ListeningPort());
        strStatus += " conns=";
        strStatus += std::to_string(m_pNetwork->ConnectionCount());
        strStatus += " accepted=";
        strStatus += std::to_string(m_pNetwork->TotalAccepted());
        strStatus += " closed=";
        strStatus += std::to_string(m_pNetwork->TotalClosed());
    }
    else
    {
        strStatus += std::to_string(m_nPort);
    }
    return strStatus;
}

/// @brief 监听端口。
std::uint16_t CTcpServerModule::Port() const
{
    return m_nPort;
}

/// @brief 网络接口（借用指针）。
INetwork* CTcpServerModule::Network() const
{
    return m_pNetwork.Get();
}

} // namespace sc
