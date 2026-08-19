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
/// 注册网络模块（INetwork）、事件分发器（IEventDispatcher）、配置模块（IConfig）、
/// 日志模块（ILogger）与回显服务（INetworkHandler）。
bool CServerApplication::RegisterComponents()
{
    // ① 注册网络模块
    sc::IModule* network = new sc::CNetworkModule();
    if (!m_moduleManager.RegisterModule(sc::IID_INetwork(), network))
    {
        network->Release();
        return false;
    }
    network->Release(); // 管理器已持有引用

    // ② 注册事件分发器模块
    sc::IModule* events = new sc::CEventDispatcher();
    if (!m_moduleManager.RegisterModule(sc::IID_EventDispatcher(), events))
    {
        events->Release();
        return false;
    }
    events->Release(); // 管理器已持有引用

    // ③ 注册配置模块（读取配置路径，默认 servera.ini）
    sc::CConfigModule* config = new sc::CConfigModule();
    std::string configPath = ConfigPath();
    if (configPath.empty())
    {
        configPath = "servera.ini";
    }
    config->LoadFile(configPath);
    if (!m_moduleManager.RegisterModule(sc::IID_IConfig(), config))
    {
        config->Release();
        return false;
    }
    config->Release(); // 管理器已持有引用

    // ④ 注册日志模块
    sc::IModule* logger = new sc::CLoggerModule();
    if (!m_moduleManager.RegisterModule(sc::IID_ILogger(), logger))
    {
        logger->Release();
        return false;
    }
    logger->Release(); // 管理器已持有引用

    // ⑤ 注册回显服务
    CEchoService* service = new CEchoService();
    if (!m_moduleManager.RegisterModule(sc::IID_INetworkHandler(), service))
    {
        service->Release();
        return false;
    }
    m_pService = service;
    service->Release(); // 管理器已持有引用
    return true;
}

/// @brief 注册模块。
///
/// 注册顺序即初始化/启动顺序：日志 → 网络。
bool CServerApplication::RegisterModules()
{
    // ① 日志模块（通过模块管理器按接口初始化）
    sc::IModule* logger = new CServerLoggerModule(m_moduleManager);
    if (!m_moduleManager.RegisterModule(logger))
    {
        logger->Release();
        return false;
    }
    logger->Release(); // 管理器已持有引用

    // ② 网络模块
    sc::IModule* network = new CServerNetworkModule(m_moduleManager, m_pService, m_nPort);
    if (!m_moduleManager.RegisterModule(network))
    {
        network->Release();
        return false;
    }
    network->Release(); // 管理器已持有引用
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
