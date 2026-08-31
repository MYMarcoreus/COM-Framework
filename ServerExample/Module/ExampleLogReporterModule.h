#pragma once

#include <cstdint>
#include <string>

#include "Module/ScopedInterfacePtr.h"
#include "Config/Config.h"
#include "Infra/ITimer.h"
#include "Module/Module.h"
#include "Network/TcpClient.h"
#include "Protocol/LogProtocol.h"

namespace serverexample {

/// @brief 日志上报模块。
///
/// 作为日志生产者示例：连接 LogServer，周期性上报 ServerExample 运行状态日志。
/// 未连接时自动尝试连接；连接断开后下个周期重连。
/// 通过 ITimer 接口驱动周期上报（不直接持有 CTimerManager）。
/// 模块名 "logreporter"。
class CExampleLogReporterModule : public sc::CModule
{
public:
    explicit CExampleLogReporterModule(const common::config::CConfig& config);

    virtual ~CExampleLogReporterModule();

    // 从配置读取 LogServer 地址并解析 ITimer 接口。
    bool Initialize(const sc::CResolveContext& ctx) override;

    // 启动周期上报。
    bool Start() override;

    // 取消定时器并关闭连接。
    void Stop() override;

    // 停止并释放资源。
    void Shutdown() override;

private:
    // 周期上报回调：未连接则连接，已连接则发送状态日志。
    void ReportStatus();

    // 当前时间戳（epoch 秒）。
    static std::uint64_t Now();

    const common::config::CConfig& m_config;
    std::string m_strHost;
    std::uint16_t m_nPort;
    std::int64_t m_nIntervalMs;
    sc::ScopedInterfacePtr<sc::ITimer> m_pTimer;
    common::timer::TimerId m_tTimerId;
    common::network::CTcpClient m_client;
};

} // namespace serverexample
