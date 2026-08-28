#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "Module/ScopedInterfacePtr.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "Network/INetwork.h"
#include "Observability/IMetrics.h"

namespace common { namespace network { class CTcpServer; } }

namespace sc {

// 前置声明，减少头文件依赖。

/// @brief 网络模块。
///
/// 实现 INetwork 接口，内部使用 CTcpServer 提供 TCP 服务器能力。
/// 可作为模块注册到 CModuleManager 中。
/// 挂载指标（可选，需注册 IMetrics 模块）：network.accepted / network.conns /
/// network.closed / network.msgs。
class CNetworkModule : public CModule, public INetwork
{
public:
    CNetworkModule();

    virtual ~CNetworkModule();

    // 从初始化上下文解析可选的 IMetrics（缺失时不上报指标）。
    bool Initialize(const CResolveContext& ctx) override;

    // 模块启动：服务器由 StartTcpServer 显式启动，此处无独立动作。
    bool Start() override;

    // 启动 TCP 服务器。
    bool StartTcpServer(uint16_t nPort, INetworkHandler* pHandler) override;

    // 停止服务器。
    void Stop() override;

    // 模块关闭：停止服务器。
    void Shutdown() override;

    // 发送数据。
    bool Send(ConnectionId nId, const char* pData, size_t nLen) override;

    // 关闭连接。
    void Close(ConnectionId nId) override;
    // 挂载业务上下文。
    void* Attach(ConnectionId nId, void* pCtx) override;

    // 取回业务上下文。
    void* GetAttached(ConnectionId nId) const override;

    // 移除并返回业务上下文。
    void* Detach(ConnectionId nId) override;
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

    // 设置空闲超时（秒，0 禁用）。
    void SetIdleTimeout(uint32_t nSeconds) override;

    // 设置最大连接数上限（0 表示不限制）。
    void SetMaxConnections(size_t nMax) override;

    // 状态报告：监听端口与连接统计。
    std::string GetStatus() const override;

    SC_DECLARE_INTERFACE_MAP();

private:
    std::unique_ptr<common::network::CTcpServer> m_pServer;
    ScopedInterfacePtr<INetworkHandler> m_pHandler;
    ScopedInterfacePtr<IMetrics> m_pMetrics;
    std::map<ConnectionId, void*> m_mapConnCtx;
    mutable std::mutex m_mutex;
    uint16_t m_nPort;
};

} // namespace sc
