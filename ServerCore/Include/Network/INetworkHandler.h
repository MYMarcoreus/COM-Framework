#pragma once

#include <cstddef>
#include <string>

#include "Common/Types.h"
#include "Component/IUnknown.h"

namespace sc {

/// @brief 网络事件处理接口。
///
/// 由上层（服务器业务）实现，接收网络层的事件回调。
/// 本接口只处理连接与原始字节流，不涉及具体协议。
class INetworkHandler : public virtual IUnknown
{
public:
    // 新连接建立。
    virtual void OnAccept(ConnectionId nId, const std::string& strPeer) = 0;

    // 收到数据。
    virtual void OnData(ConnectionId nId, const char* pData, size_t nLen) = 0;

    // 连接关闭。
    virtual void OnClose(ConnectionId nId) = 0;
};

/// @brief 获取 INetworkHandler 接口标识。
inline const InterfaceId& IID_INetworkHandler()
{
    static const InterfaceId iid = "sc::INetworkHandler";
    return iid;
}

} // namespace sc
