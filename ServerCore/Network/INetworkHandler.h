#pragma once

#include <cstddef>
#include <string>

#include "Network/ConnectionId.h"
#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"

namespace sc {

/// @brief 网络事件处理接口。
///
/// 由上层（服务器业务）实现，接收网络层的事件回调。
/// 本接口只处理连接与原始字节流，不涉及具体协议。
SC_INTERFACE(INetworkHandler, "sc::INetworkHandler", "5ee84f83-1e83-4708-9e12-2621199de5c2")
{
public:
    // 新连接建立。
    virtual void OnAccept(ConnectionId nId, const std::string& strPeer) = 0;

    // 收到数据。
    virtual void OnData(ConnectionId nId, const char* pData, size_t nLen) = 0;

    // 连接关闭。
    virtual void OnClose(ConnectionId nId) = 0;
};

} // namespace sc
