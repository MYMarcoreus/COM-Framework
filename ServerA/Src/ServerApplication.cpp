#include "ServerApplication.h"

#include <string>

#include "Event/EventDispatcher.h"
#include "Infra/IConfig.h"
#include "Infra/ILogger.h"
#include "Log/Logger.h"
#include "Module/ServerLoggerModule.h"
#include "Module/ServerNetworkModule.h"
#include "Network/NetworkComponent.h"
#include "Service/EchoService.h"

namespace servera {

/// @brief 创建 ServerA 应用程序。
///
/// @param port 监听端口。
ServerApplication::ServerApplication(std::uint16_t port)
    : port_(port), service_(nullptr),
      eventStartId_(sc::kInvalidSubscriptionId), eventStopId_(sc::kInvalidSubscriptionId)
{
}

/// @brief 销毁 ServerA 应用程序。
ServerApplication::~ServerApplication()
{
}

/// @brief 注册组件。
///
/// 注册网络组件（INetwork）、事件分发器（IEventDispatcher）、配置组件（IConfig）、
/// 日志组件（ILogger）与回显服务（INetworkHandler）。
bool ServerApplication::RegisterComponents()
{
    // ① 注册网络组件
    sc::IUnknown* network = new sc::NetworkComponent();
    if (!componentManager_.RegisterComponent(sc::IID_INetwork(), network))
    {
        network->Release();
        return false;
    }
    network->Release(); // 管理器已持有引用

    // ② 注册事件分发器组件
    sc::IUnknown* events = new sc::EventDispatcher();
    if (!componentManager_.RegisterComponent(sc::IID_EventDispatcher(), events))
    {
        events->Release();
        return false;
    }
    events->Release(); // 管理器已持有引用

    // ③ 注册配置组件（读取配置路径，默认 servera.ini）
    sc::ConfigComponent* config = new sc::ConfigComponent();
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
    sc::IUnknown* logger = new sc::LoggerComponent();
    if (!componentManager_.RegisterComponent(sc::IID_ILogger(), logger))
    {
        logger->Release();
        return false;
    }
    logger->Release(); // 管理器已持有引用

    // ⑤ 注册回显服务
    EchoService* service = new EchoService();
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
bool ServerApplication::RegisterModules()
{
    // ① 日志模块（通过组件管理器按接口初始化）
    sc::IModule* logger = new ServerLoggerModule(componentManager_);
    if (!moduleManager_.RegisterModule(logger))
    {
        logger->Release();
        return false;
    }
    logger->Release(); // 管理器已持有引用

    // ② 网络模块
    sc::IModule* network = new ServerNetworkModule(componentManager_, service_, port_);
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
bool ServerApplication::OnInitialize()
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
                common::Logger::Instance().Info(
                    "[Event] ServerA 网络已启动，端口 " + std::to_string(port));
            }
        });
    eventStopId_ = eventDispatcher_->Subscribe("network.stopped",
        [](const sc::Event&)
        {
            common::Logger::Instance().Info("[Event] ServerA 网络已停止");
        });
    return true;
}

/// @brief 启动完成钩子。
///
/// @return true。
bool ServerApplication::OnStart()
{
    return true;
}

/// @brief 关闭钩子。
///
/// 取消事件订阅并释放引用；模块的停止与关闭由 MyApplication::Shutdown 统一完成。
void ServerApplication::OnShutdown()
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
