#include "Application/TemplateApplication.h"

#include <string>

#include "Event/EventDispatcher.h"
#include "Log/Logger.h"
#include "Module/TemplateLoggerModule.h"
#include "Network/NetworkModule.h"
#include "Network/TcpServerModule.h"
#include "Service/EchoService.h"

namespace servertemplate {

/// @brief 创建 ServerTemplate 应用程序。
///
/// @param port 监听端口。
CTemplateApplication::CTemplateApplication(std::uint16_t port)
    : m_nPort(port),
      m_tEventStartId(sc::kInvalidSubscriptionId), m_tEventStopId(sc::kInvalidSubscriptionId)
{
}

/// @brief 销毁 ServerTemplate 应用程序。
CTemplateApplication::~CTemplateApplication()
{
}

/// @brief 注册模块。
///
/// 最小骨架：先调用基类默认装配（自动注册 IConfig / ILogger / IMetrics），
/// 再注册业务骨架所需模块（网络 / 事件 / 回显 / 通用 TCP 装配）。
bool CTemplateApplication::RegisterModules()
{
    // ① 基类默认装配（IConfig + ILogger + IMetrics；配置路径已由 SetConfigPath 指定）
    if (!CMyApplication::RegisterModules())
    {
        return false;
    }

    // ② 网络模块
    if (!m_moduleManager.RegisterModule(sc::IID_INetwork(), new sc::CNetworkModule()))
    {
        return false;
    }

    // ③ 事件分发器模块
    if (!m_moduleManager.RegisterModule(sc::IID_IEventDispatcher(), new sc::CEventDispatcher()))
    {
        return false;
    }

    // ④ 回显服务（按接口注册，供网络装配模块获取）
    if (!m_moduleManager.RegisterModule(
            sc::IID_INetworkHandler(), new CEchoService()))
    {
        return false;
    }

    // ⑤ 业务日志模块（通过初始化上下文按接口初始化）
    if (!m_moduleManager.RegisterModule(new CTemplateLoggerModule()))
    {
        return false;
    }

    // ⑥ 通用 TCP 服务器装配模块：从模块管理器获取网络 / 回显服务接口并启动
    if (!m_moduleManager.RegisterModule(new sc::CTcpServerModule(m_nPort)))
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
bool CTemplateApplication::OnInitialize()
{
    m_pEventDispatcher.Reset(m_moduleManager.Resolve<sc::IEventDispatcher>(sc::IID_IEventDispatcher()));
    if (m_pEventDispatcher == nullptr)
    {
        return false;
    }

    m_tEventStartId = m_pEventDispatcher->Subscribe(sc::events::kNetworkStarted,
        [](const sc::Event& event)
        {
            if (event.data != nullptr && event.size == sizeof(std::uint16_t))
            {
                std::uint16_t port = *static_cast<const std::uint16_t*>(event.data);
                common::log::CLogger::Instance().Info(
                    "[Event] ServerTemplate 网络已启动，端口 " + std::to_string(port));
            }
        });
    m_tEventStopId = m_pEventDispatcher->Subscribe(sc::events::kNetworkStopped,
        [](const sc::Event&)
        {
            common::log::CLogger::Instance().Info("[Event] ServerTemplate 网络已停止");
        });
    return true;
}

/// @brief 启动完成钩子。
///
/// @return true。
bool CTemplateApplication::OnStart()
{
    return true;
}

/// @brief 关闭钩子。
///
/// 取消事件订阅并释放引用；模块的停止与关闭由 CMyApplication::Shutdown 统一完成。
void CTemplateApplication::OnShutdown()
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

} // namespace servertemplate
