#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "Component/Component.h"
#include "Component/ScopedInterfacePtr.h"
#include "Network/INetwork.h"

namespace common { class CTcpServer; }

namespace sc {

// 前置声明，减少头文件依赖。

/// @brief 网络组件。
///
/// 实现 INetwork 接口，内部使用 CTcpServer 提供 TCP 服务器能力。
/// 可作为组件注册到 CComponentManager 中。
class CNetworkComponent : public CComponent, public INetwork
{
public:
    CNetworkComponent();

    virtual ~CNetworkComponent();

    // 启动 TCP 服务器。
    bool StartTcpServer(uint16_t nPort, INetworkHandler* pHandler) override;

    // 停止服务器。
    void Stop() override;

    // 发送数据。
    bool Send(ConnectionId nId, const char* pData, size_t nLen) override;

    // 关闭连接。
    void Close(ConnectionId nId) override;

    // 监听端口。
    uint16_t ListeningPort() const override;

    // 当前活跃连接数。
    size_t ConnectionCount() const override;

    // 累计接受连接数。
    uint64_t TotalAccepted() const override;

    // 累计关闭连接数。
    uint64_t TotalClosed() const override;

    // 指定连接是否存在。
    bool HasConnection(ConnectionId nId) const override;

    // 指定连接的对端地址。
    std::string PeerAddress(ConnectionId nId) const override;

    // 状态报告：监听端口与连接统计。
    std::string GetStatus() const override;

protected:
    // 接口查询实现。
    bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv) override;

private:
    std::unique_ptr<common::CTcpServer> m_pServer;
    ScopedInterfacePtr<INetworkHandler> m_pHandler;
    mutable std::mutex m_mutex;
    uint16_t m_nPort;
};

} // namespace sc
