#pragma once

#include <cstdint>

#include "Application/MyApplication.h"
#include "Module/ScopedInterfacePtr.h"
#include "Event/IEventDispatcher.h"

namespace logserver {

/// @brief LogServer 服务器应用程序（ServerCore 复用骨架）。
///
/// 集中收集其他服务器上报的日志并按来源写入文件：
/// - RegisterModules：默认装配（IConfig/ILogger）+ 网络/事件/服务接口模块
///   + 业务模块（日志 → 网络）。
class CLogServerApplication : public sc::CMyApplication
{
public:
    explicit CLogServerApplication(std::uint16_t nPort);

    virtual ~CLogServerApplication();

protected:
    // 注册模块：默认装配 + 接口模块（网络/事件/服务）+ 业务模块（日志/网络）。
    bool RegisterModules() override;

    // 初始化完成钩子：订阅网络生命周期事件。
    bool OnInitialize() override;

    // 启动完成钩子。
    bool OnStart() override;

    // 关闭钩子：取消订阅并释放引用。
    void OnShutdown() override;

private:
    // 从 IConfig 读取监听端口（构造传入 0 时使用）。
    std::uint16_t ResolvePort();

    std::uint16_t m_nPort;
    sc::ScopedInterfacePtr<sc::IEventDispatcher> m_pEventDispatcher;
    sc::SubscriptionId m_tEventStartId;
    sc::SubscriptionId m_tEventStopId;
    sc::SubscriptionId m_tConfigReloadId;
};

} // namespace logserver
