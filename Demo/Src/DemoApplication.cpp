#include "DemoApplication.h"

#include <string>

#include "Config/Config.h"
#include "Event/EventDispatcher.h"
#include "Log/Logger.h"
#include "Module/DemoLoggerModule.h"
#include "Module/DemoNetworkModule.h"
#include "Module/DemoTimerModule.h"
#include "Network/NetworkModule.h"
#include "Service/DemoService.h"

namespace demo {

/// @brief 创建 Demo 服务器应用程序。
///
/// @param port 监听端口；0 表示从配置文件读取。
CDemoApplication::CDemoApplication(std::uint16_t port)
    : m_nPort(port), m_pService(nullptr),
      m_tEventStartId(sc::kInvalidSubscriptionId), m_tEventStopId(sc::kInvalidSubscriptionId)
{
    // 加载配置文件（可选，best-effort）
    m_config.LoadFile("demo.ini");
    if (m_nPort == 0)
    {
        int configPort = m_config.GetInt("server.port", 9000);
        if (configPort > 0 && configPort <= 65535)
        {
            m_nPort = static_cast<std::uint16_t>(configPort);
        }
        else
        {
            m_nPort = 9000;
        }
    }
}

/// @brief 销毁 Demo 服务器应用程序。
CDemoApplication::~CDemoApplication()
{
}

/// @brief 注册模块。
///
/// 注册顺序即初始化/启动顺序：基类默认装配 → 接口模块（网络/事件/服务）→ 业务模块（日志 → 定时器 → 网络）。
/// 模块注册后由 CModuleManager 持有引用，生命周期由它统一管理。
///
/// @return true 全部注册成功；false 注册失败。
bool CDemoApplication::RegisterModules()
{
    // ① 基类默认装配（配置模块 IConfig + 日志模块 ILogger）
    if (!CMyApplication::RegisterModules())
    {
        return false;
    }

    // ② 注册网络模块
    sc::IModule* network = new sc::CNetworkModule();
    if (!m_moduleManager.RegisterModule(sc::IID_INetwork(), network))
    {
        network->Release();
        return false;
    }
    network->Release(); // 管理器已持有引用

    // ③ 注册事件分发器模块
    sc::IModule* events = new sc::CEventDispatcher();
    if (!m_moduleManager.RegisterModule(sc::IID_EventDispatcher(), events))
    {
        events->Release();
        return false;
    }
    events->Release(); // 管理器已持有引用

    // ④ 注册协议处理服务
    CDemoService* service = new CDemoService();
    if (!m_moduleManager.RegisterModule(sc::IID_INetworkHandler(), service))
    {
        service->Release();
        return false;
    }
    m_pService = service;
    service->Release(); // 管理器已持有引用

    // ⑤ 日志模块：根据配置初始化日志器
    sc::IModule* logger = new CDemoLoggerModule(m_config);
    if (!m_moduleManager.RegisterModule(logger))
    {
        logger->Release();
        return false;
    }
    logger->Release(); // 管理器已持有引用

    // ② 定时器模块：周期性输出运行状态
    int intervalMs = m_config.GetInt("timer.interval_ms", 5000);
    sc::IModule* timer = new CDemoTimerModule(intervalMs);
    if (!m_moduleManager.RegisterModule(timer))
    {
        timer->Release();
        return false;
    }
    timer->Release(); // 管理器已持有引用

    // ⑦ 网络业务模块：关联接口模块并启动 TCP 服务器
    sc::IModule* businessNetwork = new CDemoNetworkModule(m_moduleManager, m_pService, m_nPort);
    if (!m_moduleManager.RegisterModule(businessNetwork))
    {
        businessNetwork->Release();
        return false;
    }
    businessNetwork->Release(); // 管理器已持有引用
    return true;
}

/// @brief 初始化完成钩子。
///
/// 获取事件分发器，订阅网络模块发布的启动/停止事件（解耦通信验证）。
///
/// @return true。
bool CDemoApplication::OnInitialize()
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

    // 订阅网络启动事件：从事件负载读取监听端口
    m_tEventStartId = m_pEventDispatcher->Subscribe("network.started",
        [](const sc::Event& event)
        {
            if (event.data != nullptr && event.size == sizeof(std::uint16_t))
            {
                std::uint16_t port = *static_cast<const std::uint16_t*>(event.data);
                common::CLogger::Instance().Info(
                    "[Event] 收到 network.started，端口 " + std::to_string(port));
            }
        });
    // 订阅网络停止事件
    m_tEventStopId = m_pEventDispatcher->Subscribe("network.stopped",
        [](const sc::Event&)
        {
            common::CLogger::Instance().Info("[Event] 收到 network.stopped");
        });
    return true;
}

/// @brief 启动完成钩子。
///
/// 模块的启动已由 CModuleManager 在 Start 中统一完成，此处无需额外逻辑。
///
/// @return true。
bool CDemoApplication::OnStart()
{
    return true;
}

/// @brief 关闭钩子。
///
/// 取消事件订阅并释放引用；模块的停止与关闭由 CMyApplication::Shutdown
/// 中的 CModuleManager 统一完成。
void CDemoApplication::OnShutdown()
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

} // namespace demo
