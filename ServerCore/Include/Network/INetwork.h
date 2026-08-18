#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Common/Types.h"
#include "Component/IUnknown.h"
#include "Network/INetworkHandler.h"

namespace sc {

/// @brief 网络组件接口。
///
/// 提供 TCP 服务器的启动、停止、发送与关闭能力。
/// 只提供通信基础设施，不包含具体通信协议。
class INetwork : public virtual IUnknown
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
};

/// @brief 获取 INetwork 接口标识。
inline const InterfaceId& IID_INetwork()
{
    static const InterfaceId iid = "sc::INetwork";
    return iid;
}

} // namespace sc
