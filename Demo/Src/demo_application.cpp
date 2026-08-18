#include "demo_application.h"

#include <string>

#include "Config/config.h"
#include "Event/event_dispatcher.h"
#include "Log/logger.h"
#include "Module/demo_logger_module.h"
#include "Module/demo_network_module.h"
#include "Module/demo_timer_module.h"
#include "Network/network_component.h"
#include "Service/demo_service.h"

namespace demo {

/// @brief 创建 Demo 服务器应用程序。
///
/// @param port 监听端口；0 表示从配置文件读取。
CDemoApplication::CDemoApplication(std::uint16_t port)
    : port_(port), service_(nullptr),
      eventStartId_(sc::kInvalidSubscriptionId), eventStopId_(sc::kInvalidSubscriptionId)
{
    // 加载配置文件（可选，best-effort）
    config_.LoadFile("demo.ini");
    if (port_ == 0)
    {
        int configPort = config_.GetInt("server.port", 9000);
        if (configPort > 0 && configPort <= 65535)
        {
            port_ = static_cast<std::uint16_t>(configPort);
        }
        else
        {
            port_ = 9000;
        }
    }
}

/// @brief 销毁 Demo 服务器应用程序。
CDemoApplication::~CDemoApplication()
{
}

/// @brief 注册组件。
///
/// 注册网络组件（INetwork）、事件分发器（IEventDispatcher）与协议处理服务（INetworkHandler）。
bool CDemoApplication::RegisterComponents()
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

    // ③ 注册协议处理服务
    CDemoService* service = new CDemoService();
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
/// 注册顺序即初始化/启动顺序：日志 → 定时器 → 网络。
/// 模块注册后由 CModuleManager 持有引用，生命周期由它统一管理。
///
/// @return true 全部注册成功；false 注册失败。
bool CDemoApplication::RegisterModules()
{
    // ① 日志模块：根据配置初始化日志器
    sc::IModule* logger = new CDemoLoggerModule(config_);
    if (!moduleManager_.RegisterModule(logger))
    {
        logger->Release();
        return false;
    }
    logger->Release(); // 管理器已持有引用

    // ② 定时器模块：周期性输出运行状态
    int intervalMs = config_.GetInt("timer.interval_ms", 5000);
    sc::IModule* timer = new CDemoTimerModule(intervalMs);
    if (!moduleManager_.RegisterModule(timer))
    {
        timer->Release();
        return false;
    }
    timer->Release(); // 管理器已持有引用

    // ③ 网络模块：关联组件并启动 TCP 服务器
    sc::IModule* network = new CDemoNetworkModule(componentManager_, service_, port_);
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
/// 获取事件分发器，订阅网络模块发布的启动/停止事件（解耦通信验证）。
///
/// @return true。
bool CDemoApplication::OnInitialize()
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

    // 订阅网络启动事件：从事件负载读取监听端口
    eventStartId_ = eventDispatcher_->Subscribe("network.started",
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
    eventStopId_ = eventDispatcher_->Subscribe("network.stopped",
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

} // namespace demo
