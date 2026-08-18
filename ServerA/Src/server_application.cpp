#include "server_application.h"

#include <string>

#include "Event/event_dispatcher.h"
#include "Infra/i_config.h"
#include "Infra/i_logger.h"
#include "Log/logger.h"
#include "Module/server_logger_module.h"
#include "Module/server_network_module.h"
#include "Network/network_component.h"
#include "Service/echo_service.h"

namespace servera {

/// @brief 创建 ServerA 应用程序。
///
/// @param port 监听端口。
CServerApplication::CServerApplication(std::uint16_t port)
    : port_(port), service_(nullptr),
      eventStartId_(sc::kInvalidSubscriptionId), eventStopId_(sc::kInvalidSubscriptionId)
{
}

/// @brief 销毁 ServerA 应用程序。
CServerApplication::~CServerApplication()
{
}

/// @brief 注册组件。
///
/// 注册网络组件（INetwork）、事件分发器（IEventDispatcher）、配置组件（IConfig）、
/// 日志组件（ILogger）与回显服务（INetworkHandler）。
bool CServerApplication::RegisterComponents()
{
    // ① 注册网络组件
    sc::IUnknown* network = new sc::CNetworkComponent();
    if (!componentManager_.RegisterComponent(sc::IID_INetwork(), network))
    {
        network->Release();
        return false;
    }
    network->Release(); // 管理器已持有引用

    // ② 注册事件分发器组件
    sc::IUnknown* events = new sc::CEventDispatcher();
    if (!componentManager_.RegisterComponent(sc::IID_EventDispatcher(), events))
    {
        events->Release();
        return false;
    }
    events->Release(); // 管理器已持有引用

    // ③ 注册配置组件（读取配置路径，默认 servera.ini）
    sc::CConfigComponent* config = new sc::CConfigComponent();
    std::string configPath = ConfigPath();
    if (configPath.empty())
    {
        configPath = "servera.ini";
    }
    config->LoadFile(configPath);
    if (!componentManager_.RegisterComponent(sc::IID_IConfig(), config))
    {
        config->Release();
        return false;
    }
    config->Release(); // 管理器已持有引用

    // ④ 注册日志组件
    sc::IUnknown* logger = new sc::CLoggerComponent();
    if (!componentManager_.RegisterComponent(sc::IID_ILogger(), logger))
    {
        logger->Release();
        return false;
    }
    logger->Release(); // 管理器已持有引用

    // ⑤ 注册回显服务
    CEchoService* service = new CEchoService();
    if (!componentManager_.RegisterComponent(sc::IID_INetworkHandler(), service))
    {
        service->Release();
        return false;
    }
    service_ = service;
    service->Release(); // 管理器已持有引用
    return true;
}

/// @brief 注册模块。
///
/// 注册顺序即初始化/启动顺序：日志 → 网络。
bool CServerApplication::RegisterModules()
{
    // ① 日志模块（通过组件管理器按接口初始化）
    sc::IModule* logger = new CServerLoggerModule(componentManager_);
    if (!moduleManager_.RegisterModule(logger))
    {
        logger->Release();
        return false;
    }
    logger->Release(); // 管理器已持有引用

    // ② 网络模块
    sc::IModule* network = new CServerNetworkModule(componentManager_, service_, port_);
    if (!moduleManager_.RegisterModule(network))
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
    sc::IUnknown* eventObject = componentManager_.GetComponent(sc::IID_EventDispatcher());
    if (eventObject == nullptr)
    {
        return false;
    }
    void* raw = nullptr;
    if (!eventObject->QueryInterface(sc::IID_EventDispatcher(), &raw))
    {
        return false;
    }
    eventDispatcher_.Reset(static_cast<sc::IEventDispatcher*>(raw));

    eventStartId_ = eventDispatcher_->Subscribe("network.started",
        [](const sc::Event& event)
        {
            if (event.data != nullptr && event.size == sizeof(std::uint16_t))
            {
                std::uint16_t port = *static_cast<const std::uint16_t*>(event.data);
                common::CLogger::Instance().Info(
                    "[Event] ServerA 网络已启动，端口 " + std::to_string(port));
            }
        });
    eventStopId_ = eventDispatcher_->Subscribe("network.stopped",
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
    if (eventDispatcher_ != nullptr)
    {
        if (eventStartId_ != sc::kInvalidSubscriptionId)
        {
            eventDispatcher_->Unsubscribe(eventStartId_);
        }
        if (eventStopId_ != sc::kInvalidSubscriptionId)
        {
            eventDispatcher_->Unsubscribe(eventStopId_);
        }
        eventDispatcher_.Reset();
    }
}

} // namespace servera
