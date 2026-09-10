#include "Application/ExampleApplication.h"

#include <memory>
#include <string>

#include "Async/Promise.h"
#include "Config/Config.h"
#include "Event/EventDispatcher.h"
#include "Infra/AsyncExecutorModule.h"
#include "Infra/TimerModule.h"
#include "Log/Logger.h"
#include "Message/MessageRouter.h"
#include "Module/ExampleAsyncModule.h"
#include "Module/ExampleDbModule.h"
#include "Module/ExampleLoggerModule.h"
#include "Module/ExampleTimerModule.h"
#include "Module/IUserService.h"
#include "Module/IUserTable.h"
#include "Network/NetworkModule.h"
#include "Network/TcpServerModule.h"
#include "Service/ExampleService.h"

namespace serverexample {

namespace {

namespace no = common::async;

}  // namespace

/// @brief 创建 ServerExample 服务器应用程序。
///
/// @param port 监听端口；0 表示从配置文件读取。
CExampleApplication::CExampleApplication(std::uint16_t port)
    : m_nPort(port), m_tEventStartId(sc::kInvalidSubscriptionId), m_tEventStopId(sc::kInvalidSubscriptionId)
{
    // 加载配置文件（可选，best-effort）
    m_config.LoadFile("example.ini");
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

/// @brief 销毁 ServerExample 服务器应用程序。
CExampleApplication::~CExampleApplication() {}

/// @brief 注册模块。
///
/// 注册顺序即初始化/启动顺序：基类默认装配 → 接口模块（网络/事件/服务）→ 业务模块（日志 → 定时器 → 网络）。
/// 模块注册后由 CModuleManager 持有引用，生命周期由它统一管理。
///
/// @return true 全部注册成功；false 注册失败。
bool CExampleApplication::RegisterModules()
{
    // ① 基类默认装配（配置模块 IConfig + 日志模块 ILogger + 指标模块 IMetrics）
    if (!CMyApplication::RegisterModules())
    {
        return false;
    }

    // ② 异步执行器模块（供事件异步分发 / 业务重活投递；须先于事件与服务注册）
    if (!m_moduleManager.RegisterModule(sc::IID_IAsyncExecutor(), new sc::CAsyncExecutorModule(2)))
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
    if (!m_moduleManager.RegisterModule(sc::IID_INetworkHandler(), new CExampleService()))
    {
        return false;
    }

    // ⑧ 日志模块：根据配置初始化日志器
    if (!m_moduleManager.RegisterModule(new CExampleLoggerModule(m_config)))
    {
        return false;
    }

    // ⑨ 定时器模块：周期性输出运行状态
    int intervalMs = m_config.GetInt("timer.interval_ms", 5000);
    if (!m_moduleManager.RegisterModule(new CExampleTimerModule(intervalMs)))
    {
        return false;
    }

    // ⑩ 数据访问模块（模拟数据库）：对外提供用户信息表读写改删的异步函数，
    //     须先于依赖它的业务模块注册（拓扑排序保证先初始化 / 启动）。
    int dbLatencyMs = m_config.GetInt("db.latency_ms", 3);
    if (!m_moduleManager.RegisterModule(IID_IUserTable(), new CExampleDbModule(dbLatencyMs)))
    {
        return false;
    }

    // ⑪ 用户业务模块：按 IUserService 对外提供多个异步函数（读 / 写 / 改 / 删用户），
    //     内部调用本模块与数据访问模块的异步函数，并由定时器周期演示。
    int asyncIntervalMs = m_config.GetInt("async.interval_ms", 5000);
    if (!m_moduleManager.RegisterModule(IID_IUserService(), new CExampleAsyncModule(asyncIntervalMs)))
    {
        return false;
    }

    // ⑫ 通用 TCP 服务器装配模块：从模块管理器获取网络 / 服务接口并启动
    if (!m_moduleManager.RegisterModule(new sc::CTcpServerModule(m_nPort)))
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
bool CExampleApplication::OnInitialize()
{
    m_pEventDispatcher.Reset(m_moduleManager.Resolve<sc::IEventDispatcher>(sc::IID_IEventDispatcher()));
    if (m_pEventDispatcher == nullptr)
    {
        return false;
    }

    // 订阅网络启动事件：从事件负载读取监听端口
    m_tEventStartId = m_pEventDispatcher->Subscribe(sc::events::kNetworkStarted, [](const sc::Event& event)
    {
        if (event.data != nullptr && event.size == sizeof(std::uint16_t))
        {
            std::uint16_t port = *static_cast<const std::uint16_t*>(event.data);
            common::log::CLogger::Instance().Info("[Event] 收到 network.started，端口 " + std::to_string(port));
        }
    });
    // 订阅网络停止事件
    m_tEventStopId = m_pEventDispatcher->Subscribe(sc::events::kNetworkStopped, [](const sc::Event&)
    { common::log::CLogger::Instance().Info("[Event] 收到 network.stopped"); });
    // 订阅自定义事件（由 OnStart 中 PublishAsync 异步发布，工作线程处理）
    m_tExampleEventId = m_pEventDispatcher->Subscribe("example.hello", [](const sc::Event&)
    { common::log::CLogger::Instance().Info("[Event] 收到 example.hello（异步分发）"); });
    return true;
}

/// @brief 启动完成钩子。
///
/// 模块的启动已由 CModuleManager 在 Start 中统一完成，此处无需额外逻辑。
/// 示范异步事件分发：PublishAsync 将事件投递到异步执行器线程处理，
/// 不阻塞当前（启动）线程。
///
/// @return true。
bool CExampleApplication::OnStart()
{
    if (m_pEventDispatcher != nullptr)
    {
        m_pEventDispatcher->PublishAsync("example.hello", nullptr, 0);
    }

    // 模块外部（应用层）按接口调用业务模块的异步函数：只注册完成回调，不阻塞启动流程。
    m_pUserService.Reset(m_moduleManager.Resolve<IUserService>(IID_IUserService()));
    if (m_pUserService != nullptr)
    {
        CUserRecord recApp;
        recApp.strName = "app-user";
        recApp.strMail = "app-user@example.com";
        recApp.nLevel = 1;

        // 异步函数立即返回 promise 句柄：操作数据从上下文取（回调捕获上下文保活）。
        no::CPromise<CUserOpContext> promise = m_pUserService->RegisterUserAsync(recApp);
        std::shared_ptr<CUserOpContext> spCtx = promise.GetContext();
        promise.OnSettled([spCtx](no::CPromiseResult result)
        {
            common::log::CLogger::Instance().Info("[应用] 外部异步调用完成：" +
                                                  std::string(result.IsFulfilled() ? "兑现" : "拒绝") +
                                                  " id=" + std::to_string(spCtx->nUserId) + " 轨迹=" + spCtx->strTrace);
        });
        common::log::CLogger::Instance().Info("[应用] 已按接口调用业务模块异步函数（回调通知，不阻塞）");
    }
    return true;
}

/// @brief 关闭钩子。
///
/// 取消事件订阅并释放引用；模块的停止与关闭由 CMyApplication::Shutdown
/// 中的 CModuleManager 统一完成。
void CExampleApplication::OnShutdown()
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
        if (m_tExampleEventId != sc::kInvalidSubscriptionId)
        {
            m_pEventDispatcher->Unsubscribe(m_tExampleEventId);
        }
        m_pEventDispatcher.Reset();
    }
    m_pUserService.Reset();
}

}  // namespace serverexample
