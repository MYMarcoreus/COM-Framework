#include "DemoApplication.h"

#include "Config/Config.h"
#include "Module/DemoLoggerModule.h"
#include "Module/DemoNetworkModule.h"
#include "Module/DemoTimerModule.h"
#include "Network/NetworkComponent.h"
#include "Service/DemoService.h"

namespace demo {

/// @brief 创建 Demo 服务器应用程序。
///
/// @param port 监听端口；0 表示从配置文件读取。
DemoApplication::DemoApplication(std::uint16_t port)
    : port_(port), service_(nullptr)
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
DemoApplication::~DemoApplication()
{
}

/// @brief 注册组件。
///
/// 注册网络组件（INetwork）与协议处理服务（INetworkHandler）。
bool DemoApplication::RegisterComponents()
{
    // ① 注册网络组件
    sc::IUnknown* network = new sc::NetworkComponent();
    if (!componentManager_.RegisterComponent(sc::IID_INetwork(), network))
    {
        network->Release();
        return false;
    }
    network->Release(); // 管理器已持有引用

    // ② 注册协议处理服务
    DemoService* service = new DemoService();
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
/// 模块注册后由 ModuleManager 持有引用，生命周期由它统一管理。
///
/// @return true 全部注册成功；false 注册失败。
bool DemoApplication::RegisterModules()
{
    // ① 日志模块：根据配置初始化日志器
    sc::IModule* logger = new DemoLoggerModule(config_);
    if (!moduleManager_.RegisterModule(logger))
    {
        logger->Release();
        return false;
    }
    logger->Release(); // 管理器已持有引用

    // ② 定时器模块：周期性输出运行状态
    int intervalMs = config_.GetInt("timer.interval_ms", 5000);
    sc::IModule* timer = new DemoTimerModule(intervalMs);
    if (!moduleManager_.RegisterModule(timer))
    {
        timer->Release();
        return false;
    }
    timer->Release(); // 管理器已持有引用

    // ③ 网络模块：关联组件并启动 TCP 服务器
    sc::IModule* network = new DemoNetworkModule(componentManager_, service_, port_);
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
/// 模块的初始化已由 ModuleManager 在 Initialize 中统一完成，此处无需额外逻辑。
///
/// @return true。
bool DemoApplication::OnInitialize()
{
    return true;
}

/// @brief 启动完成钩子。
///
/// 模块的启动已由 ModuleManager 在 Start 中统一完成，此处无需额外逻辑。
///
/// @return true。
bool DemoApplication::OnStart()
{
    return true;
}

/// @brief 关闭钩子。
///
/// 模块的停止与关闭由 MyApplication::Shutdown 中的 ModuleManager 统一完成。
void DemoApplication::OnShutdown()
{
}

} // namespace demo
