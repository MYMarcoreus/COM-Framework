#include "DemoApplication.h"

#include <string>

#include "Config/Config.h"
#include "Log/Logger.h"
#include "Network/NetworkComponent.h"
#include "Service/DemoService.h"
#include "Timer/TimerManager.h"

namespace demo {

/// @brief 创建 Demo 服务器应用程序。
///
/// @param port 监听端口；0 表示从配置文件读取。
DemoApplication::DemoApplication(std::uint16_t port)
    : port_(port), timerId_(common::kInvalidTimerId), service_(nullptr)
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

/// @brief 初始化完成钩子。
///
/// 通过组件管理器获取网络接口与处理接口，建立关联。
bool DemoApplication::OnInitialize()
{
    // ① 配置日志器（读取 Config）
    common::Logger& logger = common::Logger::Instance();
    std::string level = config_.GetString("log.level", "info");
    if (level == "trace")
    {
        logger.SetLevel(common::LogLevel::kTrace);
    }
    else if (level == "debug")
    {
        logger.SetLevel(common::LogLevel::kDebug);
    }
    else if (level == "warn")
    {
        logger.SetLevel(common::LogLevel::kWarn);
    }
    else if (level == "error")
    {
        logger.SetLevel(common::LogLevel::kError);
    }
    else
    {
        logger.SetLevel(common::LogLevel::kInfo);
    }
    std::string logFile = config_.GetString("log.file", "");
    if (!logFile.empty())
    {
        logger.OpenFile(logFile);
    }
    logger.Info("Demo 服务器初始化中，监听端口 " + std::to_string(port_));

    // ② 获取网络接口
    sc::IUnknown* networkObject = componentManager_.GetComponent(sc::IID_INetwork());
    if (networkObject == nullptr)
    {
        return false;
    }
    void* raw = nullptr;
    if (!networkObject->QueryInterface(sc::IID_INetwork(), &raw))
    {
        return false;
    }
    network_.Reset(static_cast<sc::INetwork*>(raw));

    // ② 获取协议处理接口
    sc::IUnknown* serviceObject = componentManager_.GetComponent(sc::IID_INetworkHandler());
    if (serviceObject == nullptr)
    {
        return false;
    }
    if (!serviceObject->QueryInterface(sc::IID_INetworkHandler(), &raw))
    {
        return false;
    }
    handler_.Reset(static_cast<sc::INetworkHandler*>(raw));

    // ③ 告诉服务网络引用，用于发送响应
    if (service_ != nullptr)
    {
        service_->SetNetwork(network_.Get());
    }
    return true;
}

/// @brief 启动钩子。
///
/// 启动 TCP 服务器并监听指定端口。
bool DemoApplication::OnStart()
{
    if (network_ == nullptr || handler_ == nullptr)
    {
        return false;
    }
    // 启动周期性定时器（演示 Timer 模块）
    int intervalMs = config_.GetInt("timer.interval_ms", 5000);
    if (intervalMs < 100)
    {
        intervalMs = 100;
    }
    if (timer_.Start())
    {
        timerId_ = timer_.AddPeriodicTimer(intervalMs,
            []()
            {
                common::Logger::Instance().Info("Demo 服务器运行中");
            });
    }
    if (!network_->StartTcpServer(port_, handler_.Get()))
    {
        timer_.Stop();
        timerId_ = common::kInvalidTimerId;
        return false;
    }
    common::Logger::Instance().Info("Demo 服务器已启动，监听端口 " + std::to_string(port_) + "，按 Ctrl+C 退出");
    return true;
}

/// @brief 关闭钩子。
///
/// 停止网络服务器并释放组件引用。
void DemoApplication::OnShutdown()
{
    timer_.Stop();
    timerId_ = common::kInvalidTimerId;
    if (network_ != nullptr)
    {
        network_->Stop();
    }
    network_.Reset();
    handler_.Reset();
    service_ = nullptr;
}

} // namespace demo
