#pragma once

#include <cstddef>

#include "Module/Module.h"
#include "Component/ScopedInterfacePtr.h"
#include "Network/INetwork.h"
#include "Network/INetworkHandler.h"

namespace servera {

/// @brief 回显服务（验证 ServerCore 网络链路）。
///
/// 实现 INetworkHandler：记录连接事件，收到数据原样返回。
/// 属于 ServerA 的业务层，不属于 ServerCore。
class CEchoService : public sc::CModule, public sc::INetworkHandler
{
public:
    CEchoService();

    virtual ~CEchoService();

    // 设置网络模块引用，用于发送响应。
    void SetNetwork(sc::INetwork* network);

    // 新连接建立。
    void OnAccept(sc::ConnectionId id, const std::string& peer) override;

    // 收到数据。
    void OnData(sc::ConnectionId id, const char* data, size_t len) override;

    // 连接关闭。
    void OnClose(sc::ConnectionId id) override;

protected:
    // 接口查询实现。
    bool QueryInterfaceImpl(const sc::InterfaceId& iid, void** ppv) override;

private:
    // 记录日志。
    void Log(const std::string& message);

    sc::ScopedInterfacePtr<sc::INetwork> m_pNetwork;
};

} // namespace servera
