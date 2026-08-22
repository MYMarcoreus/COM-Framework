#include "Service/DemoService.h"

#include <string>

#include "Log/Logger.h"
#include "Module/ResolveContext.h"

namespace demo {

/// @brief 接口查询实现（接口映射宏生成：INetworkHandler）。
SC_DEFINE_INTERFACE_MAP(CDemoService, sc::CModule, sc::INetworkHandler)

/// @brief 创建 Demo 协议处理服务。
CDemoService::CDemoService()
    : sc::CModule("service")
{
    // 声明依赖网络接口与消息路由模块：生命周期拓扑排序保证其先初始化 / 启动。
    AddDependency(sc::IID_INetwork());
    AddDependency(sc::IID_IMessageRouter());
}

/// @brief 销毁 Demo 协议处理服务。
CDemoService::~CDemoService()
{
}

/// @brief 模块启动（网络收发由网络模块驱动，服务无独立启动资源）。
bool CDemoService::Start()
{
    return true;
}

/// @brief 模块停止（服务无独立资源，无需处理）。
void CDemoService::Stop()
{
}

/// @brief 模块关闭（服务无独立资源，无需处理）。
void CDemoService::Shutdown()
{
}

/// @brief 从初始化上下文获取网络 / 消息路由 / 异步执行器 / 指标接口。
///
/// 消息路由解析后设置协议提取器并注册命令处理器。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true 网络与消息路由就绪；false 缺失。
bool CDemoService::Initialize(const sc::CResolveContext& ctx)
{
    // ① 网络接口（发送响应）
    m_pNetwork.Reset(ctx.Resolve<sc::INetwork>());
    if (m_pNetwork == nullptr)
    {
        return false;
    }

    // ② 消息流水线（协议切分 + 按命令分发）
    m_pRouter.Reset(ctx.Resolve<sc::IMessageRouter>());
    if (m_pRouter == nullptr)
    {
        return false;
    }
    m_pRouter->SetExtractor(CDemoProtocol::MakeMessageExtractor());
    m_pRouter->RegisterHandler(kCmdPing,
        [this](sc::ConnectionId id, int, const char*, size_t) { HandlePing(id); });
    m_pRouter->RegisterHandler(kCmdEcho,
        [this](sc::ConnectionId id, int, const char* payload, size_t payloadSize)
        { HandleEcho(id, payload, payloadSize); });

    // ③ 异步执行器（可选，重活投递）
    m_pExecutor.Reset(ctx.Resolve<sc::IAsyncExecutor>());

    // ④ 指标（可选）
    m_pMetrics.Reset(ctx.Resolve<sc::IMetrics>());
    return true;
}

/// @brief 新连接建立回调。
///
/// 示范连接级上下文：为连接挂载业务上下文，连接关闭时取回清理。
void CDemoService::OnAccept(sc::ConnectionId id, const std::string& peer)
{
    Log("连接建立: id=" + std::to_string(id) + " peer=" + peer);
    if (m_pNetwork != nullptr)
    {
        m_pNetwork->Attach(id, new ConnContext(peer));
    }
}

/// @brief 收到数据回调。
///
/// 转发给消息流水线（缓冲 / 半包 / 粘包由 CMessageRouter 处理），
/// 并更新连接上下文与指标。
void CDemoService::OnData(sc::ConnectionId id, const char* data, size_t len)
{
    if (m_pRouter != nullptr)
    {
        m_pRouter->OnData(id, data, len);
    }
    if (m_pNetwork != nullptr)
    {
        ConnContext* pCtx = static_cast<ConnContext*>(m_pNetwork->GetAttached(id));
        if (pCtx != nullptr)
        {
            pCtx->nBytesReceived += len;
        }
    }
    if (m_pMetrics != nullptr)
    {
        m_pMetrics->Inc("demo.msgs");
    }
}

/// @brief 连接关闭回调。
///
/// 清理消息流水线缓冲，取回并释放连接上下文。
void CDemoService::OnClose(sc::ConnectionId id)
{
    if (m_pRouter != nullptr)
    {
        m_pRouter->OnClose(id);
    }
    if (m_pNetwork != nullptr)
    {
        ConnContext* pCtx = static_cast<ConnContext*>(m_pNetwork->Detach(id));
        if (pCtx != nullptr)
        {
            Log("连接关闭: id=" + std::to_string(id) + " peer=" + pCtx->strPeer +
                " 共接收 " + std::to_string(pCtx->nBytesReceived) + " 字节");
            delete pCtx;
        }
    }
}

/// @brief 处理 PING 命令。
///
/// 重活示范：通过异步执行器投递处理并返回 PONG，不阻塞网络线程。
/// 未配置异步执行器时退化为同步处理。
void CDemoService::HandlePing(sc::ConnectionId id)
{
    if (m_pExecutor != nullptr)
    {
        auto spSelf = Self<CDemoService>();
        m_pExecutor->Post(
            [spSelf, id]()
            {
                if (!spSelf)
                {
                    return;
                }
                std::string response = CDemoProtocol::BuildPong();
                if (spSelf->m_pNetwork != nullptr)
                {
                    spSelf->m_pNetwork->Send(id, response.data(), response.size());
                }
                spSelf->Log("异步处理 PING，返回 PONG: id=" + std::to_string(id));
            });
        return;
    }
    std::string response = CDemoProtocol::BuildPong();
    Log("收到 PING，返回 PONG: id=" + std::to_string(id));
    if (m_pNetwork != nullptr)
    {
        m_pNetwork->Send(id, response.data(), response.size());
    }
}

/// @brief 处理 ECHO 命令。
///
/// 重活示范：拷贝负载后通过异步执行器投递回显，不阻塞网络线程。
void CDemoService::HandleEcho(sc::ConnectionId id, const char* payload, size_t payloadSize)
{
    std::string strPayload(payload != nullptr ? payload : "", payloadSize);
    if (m_pExecutor != nullptr)
    {
        auto spSelf = Self<CDemoService>();
        m_pExecutor->Post(
            [spSelf, id, strPayload]()
            {
                if (!spSelf)
                {
                    return;
                }
                std::string response = CDemoProtocol::BuildPacket(kCmdEcho, strPayload);
                if (spSelf->m_pNetwork != nullptr)
                {
                    spSelf->m_pNetwork->Send(id, response.data(), response.size());
                }
                if (spSelf->m_pMetrics != nullptr)
                {
                    spSelf->m_pMetrics->Inc("demo.echo");
                }
                spSelf->Log("异步处理 ECHO: id=" + std::to_string(id) +
                    " len=" + std::to_string(strPayload.size()));
            });
        return;
    }
    std::string response = CDemoProtocol::BuildPacket(kCmdEcho, strPayload);
    Log("收到 ECHO，负载长度=" + std::to_string(strPayload.size()) + " id=" + std::to_string(id));
    if (m_pNetwork != nullptr)
    {
        m_pNetwork->Send(id, response.data(), response.size());
    }
}

/// @brief 状态报告。
std::string CDemoService::GetStatus() const
{
    return "service";
}

/// @brief 记录日志。
void CDemoService::Log(const std::string& message)
{
    common::CLogger::Instance().Info("[CDemoService] " + message);
}

} // namespace demo
