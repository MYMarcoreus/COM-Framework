#include "Service/LogService.h"

#include <string>

#include "Infra/IConfig.h"
#include "Log/Logger.h"
#include "Module/ResolveContext.h"

namespace logserver {

/// @brief 接口查询实现（接口映射宏生成：INetworkHandler）。
SC_DEFINE_INTERFACE_MAP(CLogService, sc::CModule, sc::INetworkHandler)

/// @brief 创建日志收集服务。
CLogService::CLogService()
    : sc::CModule("service")
{
    // 声明依赖网络接口与消息路由模块：生命周期拓扑排序保证其先初始化 / 启动。
    AddDependency(sc::IID_INetwork());
    AddDependency(sc::IID_IMessageRouter());
}

/// @brief 销毁日志收集服务。
CLogService::~CLogService()
{
}

/// @brief 模块启动（网络收发由网络模块驱动，服务无独立启动资源）。
bool CLogService::Start()
{
    return true;
}

/// @brief 模块停止（服务无独立资源，无需处理）。
void CLogService::Stop()
{
}

/// @brief 模块关闭（服务无独立资源，无需处理）。
void CLogService::Shutdown()
{
}

/// @brief 从初始化上下文获取网络 / 消息路由接口并应用存储配置。
///
/// 消息路由解析后设置协议提取器并注册命令处理器。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true 网络与消息路由就绪；false 缺失。
bool CLogService::Initialize(const sc::CResolveContext& ctx)
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
    m_pRouter->SetExtractor(CLogProtocol::MakeMessageExtractor());
    m_pRouter->RegisterHandler(kCmdSubmitLog,
        [this](sc::ConnectionId id, int, const char* pPayload, size_t nPayloadSize)
        { HandleSubmit(id, pPayload, nPayloadSize); });
    m_pRouter->RegisterHandler(kCmdPing,
        [this](sc::ConnectionId id, int, const char*, size_t) { HandlePing(id); });

    // ③ 应用存储配置
    ApplyStorageConfig(ctx);
    return true;
}

/// @brief 从初始化上下文的 IConfig 读取存储目录并设置存储。
void CLogService::ApplyStorageConfig(const sc::CResolveContext& ctx)
{
    std::string strDir = "logs";
    sc::IConfig* pConfig = ctx.Resolve<sc::IConfig>();
    if (pConfig != nullptr)
    {
        strDir = pConfig->GetString("storage.dir", "logs");
    }
    if (!m_storage.SetDirectory(strDir))
    {
        Log("设置存储目录失败: " + strDir);
    }
}

/// @brief 新连接建立回调。
void CLogService::OnAccept(sc::ConnectionId id, const std::string& strPeer)
{
    Log("日志上报连接建立: id=" + std::to_string(id) + " peer=" + strPeer);
}

/// @brief 收到数据回调。
///
/// 转发给消息流水线（缓冲 / 半包 / 粘包由 CMessageRouter 处理）。
void CLogService::OnData(sc::ConnectionId id, const char* pData, size_t nLen)
{
    if (m_pRouter != nullptr)
    {
        m_pRouter->OnData(id, pData, nLen);
    }
}

/// @brief 连接关闭回调。
///
/// 清理消息流水线缓冲。
void CLogService::OnClose(sc::ConnectionId id)
{
    Log("日志上报连接关闭: id=" + std::to_string(id));
    if (m_pRouter != nullptr)
    {
        m_pRouter->OnClose(id);
    }
}

/// @brief 状态报告。
///
/// @return 形如 "service:dir=logs files=3" 的状态描述。
std::string CLogService::GetStatus() const
{
    std::string strStatus = "service:dir=";
    strStatus += m_storage.Directory();
    strStatus += " files=";
    strStatus += std::to_string(m_storage.FileCount());
    return strStatus;
}

/// @brief 处理 kCmdSubmitLog：解码日志记录并落盘。
void CLogService::HandleSubmit(sc::ConnectionId id, const char* pPayload, size_t nPayloadSize)
{
    std::string strPayload(pPayload != nullptr ? pPayload : "", nPayloadSize);
    LogRecord record;
    if (!CLogProtocol::DecodeRecord(strPayload, &record))
    {
        Log("日志报文解码失败: id=" + std::to_string(id));
        return;
    }
    if (!m_storage.Write(record))
    {
        Log("日志落盘失败: id=" + std::to_string(id) +
            " source=" + record.strSource);
    }
}

/// @brief 处理 kCmdPing：返回 PONG。
void CLogService::HandlePing(sc::ConnectionId id)
{
    std::string strResponse = CLogProtocol::BuildPong();
    if (m_pNetwork != nullptr)
    {
        m_pNetwork->Send(id, strResponse.data(), strResponse.size());
    }
}

/// @brief 记录服务器自身日志。
void CLogService::Log(const std::string& strMessage)
{
    common::log::CLogger::Instance().Info("[CLogService] " + strMessage);
}

} // namespace logserver
