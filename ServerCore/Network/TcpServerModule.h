#pragma once

#include <cstdint>

#include "Module/ScopedInterfacePtr.h"
#include "Event/IEventDispatcher.h"
#include "Module/Module.h"
#include "Module/ModuleManager.h"
#include "Network/INetwork.h"
#include "Network/INetworkHandler.h"

namespace sc {

/// @brief 通用 TCP 服务器装配模块。
///
/// 从模块管理器获取 INetwork / INetworkHandler / IEventDispatcher 并建立关联，
/// 启动 TCP 服务器并发布生命周期事件；业务服务器无需重复实现网络装配。
///
/// 模块声明依赖 INetwork 接口模块（CModuleManager 拓扑排序自动保证其先初始化/启动）。
/// 模块名默认 "network"，同一应用需要多个 TCP 服务器时可自定义名称。
class CTcpServerModule : public CModule
{
public:
    // 创建 TCP 服务器装配模块。
    // @param nPort   监听端口。
    // @param strName 模块名（默认 "network"）。
    CTcpServerModule(std::uint16_t nPort, const char* strName = "network");

    virtual ~CTcpServerModule();

    // 从初始化上下文获取网络接口并建立关联。
    bool Initialize(const CResolveContext& ctx) override;

    // 启动 TCP 服务器，并发布启动事件。
    bool Start() override;

    // 停止服务器，并发布停止事件。
    void Stop() override;

    // 停止服务器并释放引用。
    void Shutdown() override;

    // 状态报告：监听端口与连接统计。
    std::string GetStatus() const override;

    // 监听端口。
    std::uint16_t Port() const;

    // 网络接口（借用指针；Initialize 后可用）。
    INetwork* Network() const;

private:
    std::uint16_t m_nPort;
    ScopedInterfacePtr<INetwork> m_pNetwork;
    ScopedInterfacePtr<INetworkHandler> m_pHandler;
    ScopedInterfacePtr<IEventDispatcher> m_pEventDispatcher;
};

} // namespace sc
