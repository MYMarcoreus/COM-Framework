#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Network/ConnectionId.h"
#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"
#include "Network/INetworkHandler.h"

namespace sc {

/// @brief 网络模块接口。
///
/// 提供 TCP 服务器的启动、停止、发送与关闭能力。
/// 只提供通信基础设施，不包含具体通信协议。
SC_INTERFACE(INetwork, "sc::INetwork", "74d3ba11-ac29-4ade-8cb7-f7bffa45df3f")
{
public:
    // 启动 TCP 服务器并监听指定端口。
    virtual bool StartTcpServer(uint16_t nPort, INetworkHandler* pHandler) = 0;

    // 停止服务器并释放连接。
    virtual void Stop() = 0;

    // 向指定连接发送数据。
    virtual bool Send(ConnectionId nId, const char* pData, size_t nLen) = 0;

    // 关闭指定连接。
    virtual void Close(ConnectionId nId) = 0;

    // 为指定连接挂载业务上下文（返回旧的上下文，无则返回 nullptr）。
    // 所有权归业务：框架仅存储引用，不负责释放；业务应在 OnClose 中
    // Detach 并清理。
    virtual void* Attach(ConnectionId nId, void* pCtx) = 0;

    // 取回指定连接挂载的业务上下文（不存在返回 nullptr）。
    virtual void* GetAttached(ConnectionId nId) const = 0;

    // 移除并返回指定连接的业务上下文（所有权交还调用方）。
    virtual void* Detach(ConnectionId nId) = 0;

    // 当前监听端口，未启动时返回 0。
    virtual uint16_t ListeningPort() const = 0;

    // 当前活跃连接数。
    virtual size_t ConnectionCount() const = 0;

    // 累计接受连接数。
    virtual uint64_t TotalAccepted() const = 0;

    // 累计关闭连接数。
    virtual uint64_t TotalClosed() const = 0;

    // 指定连接是否存在。
    virtual bool HasConnection(ConnectionId nId) const = 0;

    // 指定连接的对端地址；连接不存在时返回空串。
    virtual std::string PeerAddress(ConnectionId nId) const = 0;

    // 设置空闲超时（秒，0 禁用）；超时连接自动关闭。
    virtual void SetIdleTimeout(uint32_t nSeconds) = 0;

    // 设置最大连接数上限（0 表示不限制）；超限时新连接被拒绝。
    virtual void SetMaxConnections(size_t nMax) = 0;
};

} // namespace sc
