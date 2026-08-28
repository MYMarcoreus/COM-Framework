#include "Application/DemoApplication.h"

#include <string>

#include "Config/Config.h"
#include "Event/EventDispatcher.h"
#include "Infra/AsyncExecutorModule.h"
#include "Infra/TimerModule.h"
#include "Log/Logger.h"
#include "Message/MessageRouter.h"
#include "Module/DemoLoggerModule.h"
#include "Module/DemoAsyncModule.h"
#include "Module/DemoLogReporterModule.h"
#include "Module/DemoTimerModule.h"
#include "Network/NetworkModule.h"
#include "Network/TcpServerModule.h"
#include "Service/DemoService.h"

namespace demo {

/// @brief 创建 Demo 服务器应用程序。
///
/// @param port 监听端口；0 表示从配置文件读取。
CDemoApplication::CDemoApplication(std::uint16_t port)
    : m_nPort(port),
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
    // ① 基类默认装配（配置模块 IConfig + 日志模块 ILogger + 指标模块 IMetrics）
    if (!CMyApplication::RegisterModules())
    {
        return false;
    }

    // ② 异步执行器模块（供事件异步分发 / 业务重活投递；须先于事件与服务注册）
    if (!m_moduleManager.RegisterModule(
            sc::IID_IAsyncExecutor(), new sc::CAsyncExecutorModule(2)))
    {
        return false;
    }

    // ③ 定时器模块（供业务模块按接口使用定时能力）
    if (!m_moduleManager.RegisterModule(sc::IID_ITimer(), new sc::CTimerModule()))
    {
        return false;
    }

    // ④ 网络模块
    if (!m_moduleManager.RegisterModule(sc::IID_INetwork(), new sc::CNetworkModule()))
    {
        return false;
    }

    // ⑤ 事件分发器模块（Initialize 中解析异步执行器，支持异步发布）
    if (!m_moduleManager.RegisterModule(sc::IID_IEventDispatcher(), new sc::CEventDispatcher()))
    {
        return false;
    }

    // ⑥ 消息路由模块（协议切分 + 按命令分发，供协议处理服务使用）
    if (!m_moduleManager.RegisterModule(sc::IID_IMessageRouter(), new sc::CMessageRouter()))
    {
        return false;
    }

    // ⑦ 协议处理服务（按接口注册，供网络装配模块获取）
    if (!m_moduleManager.RegisterModule(
            sc::IID_INetworkHandler(), new CDemoService()))
    {
        return false;
    }

    // ⑧ 日志模块：根据配置初始化日志器
    if (!m_moduleManager.RegisterModule(new CDemoLoggerModule(m_config)))
    {
        return false;
    }

    // ⑨ 定时器模块：周期性输出运行状态
    int intervalMs = m_config.GetInt("timer.interval_ms", 5000);
    if (!m_moduleManager.RegisterModule(new CDemoTimerModule(intervalMs)))
    {
        return false;
    }

    // ⑩ 通用 TCP 服务器装配模块：从模块管理器获取网络 / 服务接口并启动
    if (!m_moduleManager.RegisterModule(new sc::CTcpServerModule(m_nPort)))
    {
        return false;
    }

    // ⑪ 日志上报模块：将运行状态周期上报到 LogServer
    if (!m_moduleManager.RegisterModule(new CDemoLogReporterModule(m_config)))
    {
        return false;
    }

    // ⑫ 异步框架演示模块：周期性演示 CTask 链式 / 多回调 / 异常传播
    int asyncIntervalMs = m_config.GetInt("async.interval_ms", 5000);
    if (!m_moduleManager.RegisterModule(new CDemoAsyncModule(asyncIntervalMs)))
    {
        return false;
    }
    return true;
}

/// @brief 初始化完成钩子。
///
/// 获取事件分发器，订阅网络模块发布的启动/停止事件（解耦通信验证）。
///
/// @return true。
bool CDemoApplication::OnInitialize()
{
    m_pEventDispatcher.Reset(m_moduleManager.Resolve<sc::IEventDispatcher>(sc::IID_IEventDispatcher()));
    if (m_pEventDispatcher == nullptr)
    {
        return false;
    }

    // 订阅网络启动事件：从事件负载读取监听端口
    m_tEventStartId = m_pEventDispatcher->Subscribe(sc::events::kNetworkStarted,
        [](const sc::Event& event)
        {
            if (event.data != nullptr && event.size == sizeof(std::uint16_t))
            {
                std::uint16_t port = *static_cast<const std::uint16_t*>(event.data);
                common::log::CLogger::Instance().Info(
                    "[Event] 收到 network.started，端口 " + std::to_string(port));
            }
        });
    // 订阅网络停止事件
    m_tEventStopId = m_pEventDispatcher->Subscribe(sc::events::kNetworkStopped,
        [](const sc::Event&)
        {
            common::log::CLogger::Instance().Info("[Event] 收到 network.stopped");
        });
    // 订阅自定义事件（由 OnStart 中 PublishAsync 异步发布，工作线程处理）
    m_tDemoEventId = m_pEventDispatcher->Subscribe("demo.hello",
        [](const sc::Event&)
        {
            common::log::CLogger::Instance().Info("[Event] 收到 demo.hello（异步分发）");
        });
    return true;
}

/// @brief 启动完成钩子。
///
/// 模块的启动已由 CModuleManager 在 Start 中统一完成，此处无需额外逻辑。
/// 示范异步事件分发：PublishAsync 将事件投递到异步执行器线程处理，
/// 不阻塞当前（启动）线程。
///
/// @return true。
bool CDemoApplication::OnStart()
{
    if (m_pEventDispatcher != nullptr)
    {
        m_pEventDispatcher->PublishAsync("demo.hello", nullptr, 0);
    }
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
        if (m_tDemoEventId != sc::kInvalidSubscriptionId)
        {
            m_pEventDispatcher->Unsubscribe(m_tDemoEventId);
        }
        m_pEventDispatcher.Reset();
    }
}

} // namespace demo
