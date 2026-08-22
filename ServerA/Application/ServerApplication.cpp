#include "Application/ServerApplication.h"

#include <string>

#include "Event/EventDispatcher.h"
#include "Infra/ConfigModule.h"
#include "Infra/LoggerModule.h"
#include "Log/Logger.h"
#include "Module/ServerLoggerModule.h"
#include "Network/NetworkModule.h"
#include "Network/TcpServerModule.h"
#include "Service/EchoService.h"

namespace servera {

/// @brief 创建 ServerA 应用程序。
///
/// @param port 监听端口。
CServerApplication::CServerApplication(std::uint16_t port)
    : m_nPort(port),
      m_tEventStartId(sc::kInvalidSubscriptionId), m_tEventStopId(sc::kInvalidSubscriptionId)
{
}

/// @brief 销毁 ServerA 应用程序。
CServerApplication::~CServerApplication()
{
}

/// @brief 注册模块。
///
/// 注册顺序即初始化/启动顺序：基类默认装配 → 接口模块（网络/事件/配置/日志/回显）→ 业务模块（日志 → 网络）。
bool CServerApplication::RegisterModules()
{
    // ① 网络模块
    if (!m_moduleManager.RegisterModule(sc::IID_INetwork(), new sc::CNetworkModule()))
    {
        return false;
    }

    // ② 事件分发器模块
    if (!m_moduleManager.RegisterModule(sc::IID_IEventDispatcher(), new sc::CEventDispatcher()))
    {
        return false;
    }

    // ③ 配置模块（读取配置路径，默认 servera.ini）
    sc::CConfigModule* config = new sc::CConfigModule();
    std::string configPath = ConfigPath();
    if (configPath.empty())
    {
        configPath = "servera.ini";
    }
    config->LoadFile(configPath);
    if (!m_moduleManager.RegisterModule(sc::IID_IConfig(), config))
    {
        return false;
    }

    // ④ 日志模块
    if (!m_moduleManager.RegisterModule(sc::IID_ILogger(), new sc::CLoggerModule()))
    {
        return false;
    }

    // ⑤ 回显服务（按接口注册，供网络装配模块获取）
    if (!m_moduleManager.RegisterModule(
            sc::IID_INetworkHandler(), new CEchoService()))
    {
        return false;
    }

    // ⑥ 业务日志模块（通过初始化上下文按接口初始化）
    if (!m_moduleManager.RegisterModule(new CServerLoggerModule()))
    {
        return false;
    }

    // ⑦ 通用 TCP 服务器装配模块：从模块管理器获取网络 / 回显服务接口并启动
    if (!m_moduleManager.RegisterModule(new sc::CTcpServerModule(m_nPort)))
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
bool CServerApplication::OnInitialize()
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
                std::uint16_t port = *static_cast<const std::uint16_t*>(event.data);
                common::CLogger::Instance().Info(
                    "[Event] ServerA 网络已启动，端口 " + std::to_string(port));
            }
        });
    m_tEventStopId = m_pEventDispatcher->Subscribe(sc::events::kNetworkStopped,
        [](const sc::Event&)
        {
            common::CLogger::Instance().Info("[Event] ServerA 网络已停止");
        });
    return true;
}

/// @brief 启动完成钩子。
///
/// @return true。
bool CServerApplication::OnStart()
{
    return true;
}

/// @brief 关闭钩子。
///
/// 取消事件订阅并释放引用；模块的停止与关闭由 CMyApplication::Shutdown 统一完成。
void CServerApplication::OnShutdown()
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
        m_pEventDispatcher.Reset();
    }
}

} // namespace servera
