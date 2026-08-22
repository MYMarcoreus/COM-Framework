#pragma once

#include <cstddef>
#include <string>

#include "Module/InterfaceMap.h"
#include "Module/ScopedInterfacePtr.h"
#include "Message/IMessageRouter.h"
#include "Module/Module.h"
#include "Network/INetwork.h"
#include "Network/INetworkHandler.h"
#include "Protocol/LogProtocol.h"
#include "Service/LogStorage.h"

namespace logserver {

/// @brief 日志收集服务。
///
/// 实现 INetworkHandler：接收上报的日志字节流，通过 ServerCore 消息流水线
/// （CMessageRouter）切分报文并分发；落盘由业务处理器完成。
/// 属于 LogServer 业务层，不属于 ServerCore。
class CLogService : public sc::CModule, public sc::INetworkHandler
{
public:
    CLogService();

    virtual ~CLogService();

    // 从初始化上下文获取网络 / 消息路由接口并应用存储配置。
    bool Initialize(const sc::CResolveContext& ctx) override;

    // 生命周期：网络收发由网络模块驱动，服务无独立资源。
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    // 新连接建立。
    void OnAccept(sc::ConnectionId id, const std::string& strPeer) override;

    // 收到数据（转发给消息流水线）。
    void OnData(sc::ConnectionId id, const char* pData, size_t nLen) override;

    // 连接关闭。
    void OnClose(sc::ConnectionId id) override;

    // 状态报告：存储目录与已打开文件数。
    std::string GetStatus() const override;

protected:
    // 接口查询实现（接口映射宏生成，暴露 INetworkHandler）。
    SC_DECLARE_INTERFACE_MAP();

private:
    // 处理 kCmdSubmitLog：解码日志记录并落盘。
    void HandleSubmit(sc::ConnectionId id, const char* pPayload, size_t nPayloadSize);

    // 处理 kCmdPing：返回 PONG。
    void HandlePing(sc::ConnectionId id);

    // 从初始化上下文的 IConfig 读取存储目录并设置存储。
    void ApplyStorageConfig(const sc::CResolveContext& ctx);

    // 记录服务器自身日志。
    void Log(const std::string& strMessage);

    sc::ScopedInterfacePtr<sc::INetwork> m_pNetwork;
    sc::ScopedInterfacePtr<sc::IMessageRouter> m_pRouter;
    CLogStorage m_storage;
};

} // namespace logserver
