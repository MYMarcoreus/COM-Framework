#pragma once

#include <cstdint>

#include "Application/MyApplication.h"
#include "Config/Config.h"
#include "Event/IEventDispatcher.h"
#include "Module/IUserService.h"
#include "Module/ScopedInterfacePtr.h"

namespace serverexample {

/// @brief ServerExample 服务器应用程序。
///
/// 验证 ServerCore：注册网络模块与协议处理服务，通过 CModuleManager
/// 统一管理日志 / 定时器 / 网络模块的生命周期，并通过事件分发器
/// 订阅网络模块发布的事件（模块间解耦通信）。
///
/// 业务侧另注册「模拟数据库模块（IUserTable）+ 用户业务模块（IUserService）」：
/// 业务模块对外提供多个异步函数，内部调用本模块与其他模块的异步函数（详见 Module/）。
/// 启动钩子里按接口调用一次业务模块的异步函数（回调通知），示范模块外部的异步调用方式。
class CExampleApplication : public sc::CMyApplication
{
   public:
    explicit CExampleApplication(std::uint16_t port);

    virtual ~CExampleApplication();

   protected:
    // 注册模块：默认装配 + 接口模块（网络/事件/服务）+ 业务模块（日志/定时器/网络）。
    bool RegisterModules() override;

    // 初始化完成钩子：订阅网络生命周期事件。
    bool OnInitialize() override;

    // 启动完成钩子。
    bool OnStart() override;

    // 关闭钩子：取消订阅并释放引用。
    void OnShutdown() override;

   private:
    std::uint16_t m_nPort;
    common::config::CConfig m_config;
    sc::ScopedInterfacePtr<sc::IEventDispatcher> m_pEventDispatcher;
    sc::ScopedInterfacePtr<IUserService> m_pUserService;  // 业务模块接口（模块外部异步调用示例）
    sc::SubscriptionId m_tEventStartId;
    sc::SubscriptionId m_tEventStopId;
    sc::SubscriptionId m_tExampleEventId;
};

}  // namespace serverexample
