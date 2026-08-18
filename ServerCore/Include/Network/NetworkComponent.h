#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "Component/Component.h"
#include "Component/ScopedInterfacePtr.h"
#include "Network/INetwork.h"

namespace common { class TcpServer; }

namespace sc {

// 前置声明，减少头文件依赖。

/// @brief 网络组件。
///
/// 实现 INetwork 接口，内部使用 TcpServer 提供 TCP 服务器能力。
/// 可作为组件注册到 ComponentManager 中。
class NetworkComponent : public Component, public INetwork
{
public:
    NetworkComponent();

    virtual ~NetworkComponent();

    // 启动 TCP 服务器。
    bool StartTcpServer(uint16_t port, INetworkHandler* handler) override;

    // 停止服务器。
    void Stop() override;

    // 发送数据。
    bool Send(ConnectionId id, const char* data, size_t len) override;

    // 关闭连接。
    void Close(ConnectionId id) override;

    // 监听端口。
    uint16_t ListeningPort() const override;

protected:
    // 接口查询实现。
    bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv) override;

private:
    std::unique_ptr<common::TcpServer> server_;
    ScopedInterfacePtr<INetworkHandler> handler_;
    mutable std::mutex mutex_;
    uint16_t port_;
};

} // namespace sc
