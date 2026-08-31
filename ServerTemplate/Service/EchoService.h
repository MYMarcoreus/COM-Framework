#pragma once

#include <cstddef>
#include <string>

#include "Module/InterfaceMap.h"
#include "Module/ScopedInterfacePtr.h"
#include "Module/Module.h"
#include "Module/ModuleManager.h"
#include "Network/INetwork.h"
#include "Network/INetworkHandler.h"

namespace servertemplate {

/// @brief 回显服务（验证 ServerCore 网络链路）。
///
/// 实现 INetworkHandler：记录连接事件，收到数据原样返回。
/// 属于 ServerTemplate 的业务层，不属于 ServerCore。
/// 在 Initialize 中通过模块管理器按接口获取网络模块，用于发送响应。
class CEchoService : public sc::CModule, public sc::INetworkHandler
{
public:
    CEchoService();

    virtual ~CEchoService();

    // 从初始化上下文获取网络接口。
    bool Initialize(const sc::CResolveContext& ctx) override;

    // 生命周期：网络收发由网络模块驱动，服务无独立资源。
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    // 新连接建立。
    void OnAccept(sc::ConnectionId id, const std::string& peer) override;

    // 收到数据。
    void OnData(sc::ConnectionId id, const char* data, size_t len) override;

    // 连接关闭。
    void OnClose(sc::ConnectionId id) override;

protected:
    // 接口查询实现（接口映射宏生成，暴露 INetworkHandler）。
    SC_DECLARE_INTERFACE_MAP();

private:
    // 记录日志。
    void Log(const std::string& message);

    sc::ScopedInterfacePtr<sc::INetwork> m_pNetwork;
};

} // namespace servertemplate
