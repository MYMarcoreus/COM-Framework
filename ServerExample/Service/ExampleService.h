#pragma once

#include <cstddef>
#include <string>

#include "Module/InterfaceMap.h"
#include "Module/ScopedInterfacePtr.h"
#include "Module/Module.h"
#include "Infra/IAsyncExecutor.h"
#include "Message/IMessageRouter.h"
#include "Network/INetwork.h"
#include "Network/INetworkHandler.h"
#include "Observability/IMetrics.h"
#include "Protocol/ExampleProtocol.h"

namespace serverexample {

/// @brief 每连接业务上下文（挂载到网络模块，示范连接级上下文）。
///
/// 连接建立时由 OnAccept 创建并 Attach 到网络模块；
/// 连接关闭时由 OnClose Detach 并释放。
struct ConnContext
{
    std::string strPeer;   // 对端地址
    size_t nBytesReceived; // 累计接收字节数

    explicit ConnContext(const std::string& peer)
        : strPeer(peer), nBytesReceived(0) {}
};

/// @brief Example 协议处理服务。
///
/// 实现 INetworkHandler，接收网络原始数据，通过 ServerCore 消息流水线
/// （CMessageRouter）按协议切分并分发；命令处理投递到异步执行器，
/// 不阻塞网络线程。示范：
///  - 消息流水线下沉（业务只需提取器 + 命令处理器）；
///  - 连接级上下文（Attach / GetAttached / Detach）；
///  - 重活异步投递（IAsyncExecutor）；
///  - 指标上报（IMetrics）。
class CExampleService : public sc::CModule, public sc::INetworkHandler
{
public:
    CExampleService();

    virtual ~CExampleService();

    // 从初始化上下文获取网络 / 消息路由 / 异步执行器 / 指标接口。
    bool Initialize(const sc::CResolveContext& ctx) override;

    // 生命周期：网络收发由网络模块驱动，服务无独立资源。
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    // 新连接建立。
    void OnAccept(sc::ConnectionId id, const std::string& peer) override;

    // 收到数据（转发给消息流水线）。
    void OnData(sc::ConnectionId id, const char* data, size_t len) override;

    // 连接关闭。
    void OnClose(sc::ConnectionId id) override;

    // 状态报告。
    std::string GetStatus() const override;

protected:
    // 接口查询实现（接口映射宏生成，暴露 INetworkHandler）。
    SC_DECLARE_INTERFACE_MAP();

private:
    // 处理 PING 命令（异步返回 PONG）。
    void HandlePing(sc::ConnectionId id);

    // 处理 ECHO 命令（异步回显负载）。
    void HandleEcho(sc::ConnectionId id, const char* payload, size_t payloadSize);

    // 记录日志。
    void Log(const std::string& message);

    sc::ScopedInterfacePtr<sc::INetwork> m_pNetwork;
    sc::ScopedInterfacePtr<sc::IMessageRouter> m_pRouter;
    sc::ScopedInterfacePtr<sc::IAsyncExecutor> m_pExecutor;
    sc::ScopedInterfacePtr<sc::IMetrics> m_pMetrics;
};

} // namespace serverexample
