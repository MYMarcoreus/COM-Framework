#include "Module/DemoNetworkModule.h"

#include <string>

#include "Log/Logger.h"
#include "Service/DemoService.h"

namespace demo {

/// @brief 创建网络模块。
///
/// @param componentManager 组件管理器，用于获取网络与协议处理组件。
/// @param service          协议处理服务（借用指针，由组件管理器持有）。
/// @param port             监听端口。
CDemoNetworkModule::CDemoNetworkModule(sc::CComponentManager& componentManager,
                                     CDemoService* service, std::uint16_t port)
    : sc::CModule("network"), m_componentManager(componentManager),
      m_pService(service), m_nPort(port)
{
}

/// @brief 销毁网络模块。
CDemoNetworkModule::~CDemoNetworkModule()
{
}

/// @brief 从组件管理器获取网络接口并建立关联。
///
/// 获取 INetwork、INetworkHandler 与 IEventDispatcher 接口，
/// 并将网络引用注入协议服务，使服务能够发送响应。
///
/// @return true 关联成功；false 组件缺失。
bool CDemoNetworkModule::Initialize()
{
    // ① 获取网络接口
    sc::IUnknown* networkObject = m_componentManager.GetComponent(sc::IID_INetwork());
    if (networkObject == nullptr)
    {
        return false;
    }
    void* raw = nullptr;
    if (!networkObject->QueryInterface(sc::IID_INetwork(), &raw))
    {
        return false;
    }
    m_pNetwork.Reset(static_cast<sc::INetwork*>(raw));

    // ② 获取协议处理接口
    sc::IUnknown* serviceObject = m_componentManager.GetComponent(sc::IID_INetworkHandler());
    if (serviceObject == nullptr)
    {
        return false;
    }
    if (!serviceObject->QueryInterface(sc::IID_INetworkHandler(), &raw))
    {
        return false;
    }
    m_pHandler.Reset(static_cast<sc::INetworkHandler*>(raw));

    // ③ 获取事件分发器（用于发布生命周期事件）
    sc::IUnknown* eventObject = m_componentManager.GetComponent(sc::IID_EventDispatcher());
    if (eventObject != nullptr)
    {
        if (eventObject->QueryInterface(sc::IID_EventDispatcher(), &raw))
        {
            m_pEventDispatcher.Reset(static_cast<sc::IEventDispatcher*>(raw));
        }
    }

    // ④ 注入网络引用到协议服务，用于发送响应
    if (m_pService != nullptr)
    {
        m_pService->SetNetwork(m_pNetwork.Get());
    }
    common::CLogger::Instance().Info("Demo 服务器初始化中，监听端口 " + std::to_string(m_nPort));
    return true;
}

/// @brief 启动 TCP 服务器。
///
/// 启动成功后发布 "network.started" 事件（负载为监听端口）。
///
/// @return true 启动成功；false 启动失败（如端口被占用）。
bool CDemoNetworkModule::Start()
{
    if (m_pNetwork == nullptr || m_pHandler == nullptr)
    {
        return false;
    }
    if (!m_pNetwork->StartTcpServer(m_nPort, m_pHandler.Get()))
    {
        return false;
    }
    if (m_pEventDispatcher != nullptr)
    {
        m_pEventDispatcher->Publish("network.started", &m_nPort, sizeof(m_nPort));
    }
    common::CLogger::Instance().Info(
        "Demo 服务器已启动，监听端口 " + std::to_string(m_nPort) + "，按 Ctrl+C 退出");
    return true;
}

/// @brief 停止服务器。
///
/// 停止前发布 "network.stopped" 事件。
void CDemoNetworkModule::Stop()
{
    if (m_pEventDispatcher != nullptr)
    {
        m_pEventDispatcher->Publish("network.stopped", nullptr, 0);
    }
    if (m_pNetwork != nullptr)
    {
        m_pNetwork->Stop();
    }
}

/// @brief 停止服务器并释放引用。
void CDemoNetworkModule::Shutdown()
{
    Stop();
    m_pNetwork.Reset();
    m_pHandler.Reset();
    m_pEventDispatcher.Reset();
    m_pService = nullptr;
}

} // namespace demo
