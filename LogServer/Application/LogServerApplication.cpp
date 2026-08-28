#include "Application/LogServerApplication.h"

#include <string>

#include "Event/EventDispatcher.h"
#include "Infra/ConfigReloadModule.h"
#include "Infra/IConfig.h"
#include "Log/Logger.h"
#include "Message/MessageRouter.h"
#include "Module/LogServerLoggerModule.h"
#include "Network/NetworkModule.h"
#include "Network/TcpServerModule.h"
#include "Service/LogService.h"

namespace logserver {

/// @brief 创建 LogServer 服务器应用程序。
///
/// @param nPort 监听端口；0 表示从配置文件读取。
CLogServerApplication::CLogServerApplication(std::uint16_t nPort)
    : m_nPort(nPort),
      m_tEventStartId(sc::kInvalidSubscriptionId), m_tEventStopId(sc::kInvalidSubscriptionId),
      m_tConfigReloadId(sc::kInvalidSubscriptionId)
{
    // 配置文件路径（基类默认装配的 IConfig 会在 Initialize 时加载）
    SetConfigPath("logserver.ini");
}

/// @brief 销毁 LogServer 服务器应用程序。
CLogServerApplication::~CLogServerApplication()
{
}

/// @brief 注册模块。
///
/// 注册顺序即初始化/启动顺序：基类默认装配 → 接口模块（网络/事件/服务）→ 业务模块（日志 → 网络）。
///
/// @return true 全部注册成功；false 注册失败。
bool CLogServerApplication::RegisterModules()
{
    // ① 基类默认装配（IConfig 加载 logserver.ini + ILogger）
    if (!CMyApplication::RegisterModules())
    {
        return false;
    }

    // ② 网络模块
    if (!m_moduleManager.RegisterModule(sc::IID_INetwork(), new sc::CNetworkModule()))
    {
        return false;
    }

    // ③ 事件分发器模块
    if (!m_moduleManager.RegisterModule(sc::IID_IEventDispatcher(), new sc::CEventDispatcher()))
    {
        return false;
    }

    // ④ 消息路由模块（协议切分 + 按命令分发，供日志收集服务使用）
    if (!m_moduleManager.RegisterModule(sc::IID_IMessageRouter(), new sc::CMessageRouter()))
    {
        return false;
    }

    // ⑤ 日志收集服务（按接口注册，供网络装配模块获取）
    if (!m_moduleManager.RegisterModule(
            sc::IID_INetworkHandler(), new CLogService()))
    {
        return false;
    }

    // ⑥ 业务日志模块：根据配置初始化日志器
    if (!m_moduleManager.RegisterModule(new CLogServerLoggerModule()))
    {
        return false;
    }

    // ⑦ 通用 TCP 服务器装配模块：从模块管理器获取网络 / 服务接口并启动
    if (!m_moduleManager.RegisterModule(
            new sc::CTcpServerModule(ResolvePort())))
    {
        return false;
    }

    // ⑧ 配置热加载模块：周期检测配置变更，通过事件分发器广播 config.reloaded
    if (!m_moduleManager.RegisterModule(new sc::CConfigReloadModule(2000)))
    {
        return false;
    }
    return true;
}

/// @brief 初始化完成钩子。
///
/// 获取事件分发器，订阅网络模块发布的启动/停止事件。
///
/// @return true。
bool CLogServerApplication::OnInitialize()
{
    m_pEventDispatcher.Reset(m_moduleManager.Resolve<sc::IEventDispatcher>(sc::IID_IEventDispatcher()));
    if (m_pEventDispatcher == nullptr)
    {
        return false;
    }

    m_tEventStartId = m_pEventDispatcher->Subscribe(sc::events::kNetworkStarted,
        [](const sc::Event& event)
        {
            if (event.data != nullptr && event.size == sizeof(std::uint16_t))
            {
                std::uint16_t nPort = *static_cast<const std::uint16_t*>(event.data);
                common::log::CLogger::Instance().Info(
                    "[Event] LogServer 网络已启动，端口 " + std::to_string(nPort));
            }
        });
    m_tEventStopId = m_pEventDispatcher->Subscribe(sc::events::kNetworkStopped,
        [](const sc::Event&)
        {
            common::log::CLogger::Instance().Info("[Event] LogServer 网络已停止");
        });
    // 订阅配置变更事件（由 CConfigReloadModule 广播）
    m_tConfigReloadId = m_pEventDispatcher->Subscribe(sc::events::kConfigReloaded,
        [](const sc::Event&)
        {
            common::log::CLogger::Instance().Info("[Event] 收到 config.reloaded，配置已热加载");
        });
    return true;
}

/// @brief 启动完成钩子。
///
/// @return true。
bool CLogServerApplication::OnStart()
{
    return true;
}

/// @brief 关闭钩子。
///
/// 取消事件订阅并释放引用；模块的停止与关闭由 CMyApplication::Shutdown 统一完成。
void CLogServerApplication::OnShutdown()
{
    if (m_pEventDispatcher != nullptr)
    {
        if (m_tEventStartId != sc::kInvalidSubscriptionId)
        {
            m_pEventDispatcher->Unsubscribe(m_tEventStartId);
        }
        if (m_tEventStopId != sc::kInvalidSubscriptionId)
        {
            m_pEventDispatcher->Unsubscribe(m_tEventStopId);
        }
        if (m_tConfigReloadId != sc::kInvalidSubscriptionId)
        {
            m_pEventDispatcher->Unsubscribe(m_tConfigReloadId);
        }
        m_pEventDispatcher.Reset();
    }
}

/// @brief 从 IConfig 读取监听端口。
///
/// 构造传入端口非 0 时直接使用；否则读取 server.port（默认 9200）。
///
/// @return 监听端口。
std::uint16_t CLogServerApplication::ResolvePort()
{
    if (m_nPort != 0)
    {
        return m_nPort;
    }
    std::uint16_t nPort = 9200;
    sc::IConfig* pConfig = m_moduleManager.Resolve<sc::IConfig>(sc::IID_IConfig());
    if (pConfig != nullptr)
    {
        int nConfigPort = pConfig->GetInt("server.port", 9200);
        if (nConfigPort > 0 && nConfigPort <= 65535)
        {
            nPort = static_cast<std::uint16_t>(nConfigPort);
        }
    }
    return nPort;
}

} // namespace logserver
