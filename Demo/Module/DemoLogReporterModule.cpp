#include "Module/DemoLogReporterModule.h"

#include <ctime>

#include "Infra/GuardedTimer.h"
#include "Log/Logger.h"
#include "Module/ResolveContext.h"

namespace demo {

/// @brief 创建日志上报模块。
///
/// @param config 应用配置（读取 [logserver] 段）。
CDemoLogReporterModule::CDemoLogReporterModule(const common::CConfig& config)
    : sc::CModule("logreporter"), m_config(config),
      m_nPort(9200), m_nIntervalMs(5000), m_tTimerId(common::kInvalidTimerId)
{
    // 依赖 ITimer 接口模块：生命周期拓扑排序保证其先初始化 / 启动。
    AddDependency(sc::IID_ITimer());
}

/// @brief 销毁日志上报模块。
CDemoLogReporterModule::~CDemoLogReporterModule()
{
    Stop();
}

/// @brief 从配置读取 LogServer 地址并解析 ITimer 接口。
///
/// 读取 logserver.host / logserver.port / logserver.interval_ms。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true 定时器接口就绪；false 缺失。
bool CDemoLogReporterModule::Initialize(const sc::CResolveContext& ctx)
{
    m_pTimer.Reset(ctx.Resolve<sc::ITimer>());
    if (m_pTimer == nullptr)
    {
        return false;
    }
    m_strHost = m_config.GetString("logserver.host", "127.0.0.1");
    int port = m_config.GetInt("logserver.port", 9200);
    if (port > 0 && port <= 65535)
    {
        m_nPort = static_cast<std::uint16_t>(port);
    }
    int intervalMs = m_config.GetInt("logserver.interval_ms", 5000);
    if (intervalMs >= 100)
    {
        m_nIntervalMs = intervalMs;
    }
    return true;
}

/// @brief 启动周期上报。
///
/// 注册周期性定时器，每个周期尝试连接或上报状态日志。
///
/// @return true 启动成功；false 定时器接口缺失。
bool CDemoLogReporterModule::Start()
{
    if (m_pTimer == nullptr)
    {
        return false;
    }
    // 周期定时任务：用模板守卫函数统一处理弱引用生命周期，
    // 回调参数为具体类型强引用（无需转换）。
    m_tTimerId = sc::AddGuardedPeriodicTimer(m_pTimer.Get(), m_nIntervalMs,
        WeakSelf<CDemoLogReporterModule>(),
        [](const sc::ScopedInterfacePtr<CDemoLogReporterModule>& sp)
        {
            sp->ReportStatus();
        });
    return true;
}

/// @brief 停止上报并关闭连接。
///
/// 只取消本模块注册的定时器（共享的 ITimer 线程由 TimerModule 生命周期统一管理）。
void CDemoLogReporterModule::Stop()
{
    if (m_tTimerId != common::kInvalidTimerId)
    {
        if (m_pTimer != nullptr)
        {
            m_pTimer->Cancel(m_tTimerId);
        }
        m_tTimerId = common::kInvalidTimerId;
    }
    m_client.Stop();
}

/// @brief 停止并释放资源。
void CDemoLogReporterModule::Shutdown()
{
    Stop();
    m_pTimer.Reset();
}

/// @brief 周期上报回调。
///
/// 未连接时先停止旧连接线程（幂等）再发起连接；
/// 已连接时上报一条 Demo 运行状态日志。
void CDemoLogReporterModule::ReportStatus()
{
    if (!m_client.IsConnected())
    {
        m_client.Stop(); // 确保旧连接线程已退出（未连接时幂等）
        m_client.Connect(m_strHost, m_nPort,
            [](bool /*bOk*/, const std::string& /*strPeer*/) {},
            [](const char* /*pData*/, size_t /*nLen*/) {},
            []() {});
        return;
    }

    logserver::LogRecord record;
    record.nTimestamp = Now();
    record.strLevel = "info";
    record.strSource = "demo";
    record.strContent = "Demo 服务器运行中，日志上报模块正常";
    std::string strPacket = logserver::CLogProtocol::BuildSubmit(record);
    if (!m_client.Send(strPacket.data(), strPacket.size()))
    {
        common::CLogger::Instance().Warn("[CDemoLogReporterModule] 日志上报发送失败");
    }
}

/// @brief 当前时间戳（epoch 秒）。
std::uint64_t CDemoLogReporterModule::Now()
{
    return static_cast<std::uint64_t>(std::time(nullptr));
}

} // namespace demo
