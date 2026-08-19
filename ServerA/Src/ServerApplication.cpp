#include "ServerApplication.h"

#include <string>

#include "Event/EventDispatcher.h"
#include "Infra/IConfig.h"
#include "Infra/ILogger.h"
#include "Log/Logger.h"
#include "Module/ServerLoggerModule.h"
#include "Module/ServerNetworkModule.h"
#include "Network/NetworkModule.h"
#include "Service/EchoService.h"

namespace servera {

/// @brief 创建 ServerA 应用程序。
///
/// @param port 监听端口。
CServerApplication::CServerApplication(std::uint16_t port)
    : m_nPort(port), m_pService(nullptr),
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
    if (!m_moduleManager.RegisterModuleOwned(sc::IID_INetwork(), new sc::CNetworkModule()))
    {
        return false;
    }

    // ② 事件分发器模块
    if (!m_moduleManager.RegisterModuleOwned(sc::IID_EventDispatcher(), new sc::CEventDispatcher()))
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
    if (!m_moduleManager.RegisterModuleOwned(sc::IID_IConfig(), config))
    {
        return false;
    }

    // ④ 日志模块
    if (!m_moduleManager.RegisterModuleOwned(sc::IID_ILogger(), new sc::CLoggerModule()))
    {
        return false;
    }

    // ⑤ 回显服务（保存指针用于注入业务模块）
    CEchoService* service = new CEchoService();
    if (!m_moduleManager.RegisterModuleOwned(sc::IID_INetworkHandler(), service))
    {
        return false;
    }
    m_pService = service;

    // ⑥ 业务日志模块（通过模块管理器按接口初始化）
    if (!m_moduleManager.RegisterModuleOwned(new CServerLoggerModule(m_moduleManager)))
    {
        return false;
    }

    // ⑦ 业务网络模块
    if (!m_moduleManager.RegisterModuleOwned(
            new CServerNetworkModule(m_moduleManager, m_pService, m_nPort)))
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
    sc::IUnknown* eventObject = m_moduleManager.GetModuleByIid(sc::IID_EventDispatcher());
    if (eventObject == nullptr)
    {
        return false;
    }
    void* raw = nullptr;
    if (!eventObject->QueryInterface(sc::IID_EventDispatcher(), &raw))
    {
        return false;
    }
    m_pEventDispatcher.Reset(static_cast<sc::IEventDispatcher*>(raw));

    m_tEventStartId = m_pEventDispatcher->Subscribe("network.started",
        [](const sc::Event& event)
        {
            if (event.data != nullptr && event.size == sizeof(std::uint16_t))
            {
                std::uint16_t port = *static_cast<const std::uint16_t*>(event.data);
                common::CLogger::Instance().Info(
                    "[Event] ServerA 网络已启动，端口 " + std::to_string(port));
            }
        });
    m_tEventStopId = m_pEventDispatcher->Subscribe("network.stopped",
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
