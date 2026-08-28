#include "Service/EchoService.h"

#include <string>

#include "Log/Logger.h"
#include "Module/ResolveContext.h"

namespace servera {

/// @brief 接口查询实现（接口映射宏生成：INetworkHandler）。
SC_DEFINE_INTERFACE_MAP(CEchoService, sc::CModule, sc::INetworkHandler)

/// @brief 创建回显服务。
CEchoService::CEchoService()
    : sc::CModule("echo")
{
    // 声明依赖网络接口模块：生命周期拓扑排序保证其先初始化 / 启动。
    AddDependency(sc::IID_INetwork());
}

/// @brief 销毁回显服务。
CEchoService::~CEchoService()
{
}

/// @brief 模块启动（网络收发由网络模块驱动，服务无独立启动资源）。
bool CEchoService::Start()
{
    return true;
}

/// @brief 模块停止（服务无独立资源，无需处理）。
void CEchoService::Stop()
{
}

/// @brief 模块关闭（服务无独立资源，无需处理）。
void CEchoService::Shutdown()
{
}

/// @brief 从初始化上下文获取网络接口。
///
/// 按类型自动绑定接口标识获取网络模块，用于发送响应。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true 获取成功；false 网络模块缺失。
bool CEchoService::Initialize(const sc::CResolveContext& ctx)
{
    m_pNetwork.Reset(ctx.Resolve<sc::INetwork>());
    return m_pNetwork != nullptr;
}

/// @brief 新连接建立回调。
void CEchoService::OnAccept(sc::ConnectionId id, const std::string& peer)
{
    Log("连接建立: id=" + std::to_string(id) + " peer=" + peer);
}

/// @brief 收到数据回调。
///
/// 原样返回收到的数据，验证 ServerCore 网络收发链路。
void CEchoService::OnData(sc::ConnectionId id, const char* data, size_t len)
{
    Log("收到数据: id=" + std::to_string(id) + " len=" + std::to_string(len));
    if (m_pNetwork != nullptr && data != nullptr && len > 0)
    {
        m_pNetwork->Send(id, data, len);
    }
}

/// @brief 连接关闭回调。
void CEchoService::OnClose(sc::ConnectionId id)
{
    Log("连接关闭: id=" + std::to_string(id));
}

/// @brief 记录日志。
void CEchoService::Log(const std::string& message)
{
    common::log::CLogger::Instance().Info("[CEchoService] " + message);
}

} // namespace servera
