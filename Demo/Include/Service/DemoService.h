#pragma once

#include <map>
#include <string>

#include "Component/Component.h"
#include "Component/ScopedInterfacePtr.h"
#include "Network/INetwork.h"
#include "Network/INetworkHandler.h"
#include "Protocol/DemoProtocol.h"

namespace demo {

/// @brief Demo 协议处理服务。
///
/// 实现 INetworkHandler，接收网络原始数据，按 Demo 协议解析并响应。
/// 协议解析属于 Demo，不属于 ServerCore。
class DemoService : public sc::Component, public sc::INetworkHandler
{
public:
    DemoService();

    virtual ~DemoService();

    // 设置网络组件引用，用于发送响应。
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
    // 处理一个完整报文。
    void HandlePacket(sc::ConnectionId id, const Packet& packet);

    // 记录日志。
    void Log(const std::string& message);

    sc::ScopedInterfacePtr<sc::INetwork> network_;
    std::map<sc::ConnectionId, std::string> pendingBuffers_;
};

} // namespace demo
