#pragma once

#include <cstddef>
#include <cstdint>

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
    virtual bool StartTcpServer(uint16_t port, INetworkHandler* handler) = 0;

    // 停止服务器并释放连接。
    virtual void Stop() = 0;

    // 向指定连接发送数据。
    virtual bool Send(ConnectionId id, const char* data, size_t len) = 0;

    // 关闭指定连接。
    virtual void Close(ConnectionId id) = 0;

    // 当前监听端口，未启动时返回 0。
    virtual uint16_t ListeningPort() const = 0;
};

/// @brief 获取 INetwork 接口标识。
inline const InterfaceId& IID_INetwork()
{
    static const InterfaceId iid = "sc::INetwork";
    return iid;
}

} // namespace sc
