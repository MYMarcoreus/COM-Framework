#include "Module/ExampleLoggerModule.h"

#include <string>

#include "Log/Logger.h"

namespace serverexample {

/// @brief 创建日志模块。
///
/// @param config 应用配置，用于读取日志级别与文件。
CExampleLoggerModule::CExampleLoggerModule(const common::config::CConfig& config)
    : sc::CModule("logger"), m_config(config)
{
}

/// @brief 销毁日志模块。
CExampleLoggerModule::~CExampleLoggerModule()
{
}

/// @brief 模块启动（日志器由全局单例管理，无独立启动资源）。
bool CExampleLoggerModule::Start()
{
    return true;
}

/// @brief 模块停止（全局日志单例持续运行，无需处理）。
void CExampleLoggerModule::Stop()
{
}

/// @brief 模块关闭（全局日志单例由全局管理，无需处理）。
void CExampleLoggerModule::Shutdown()
{
}

/// @brief 根据配置初始化日志器。
///
/// 读取 log.level（trace/debug/info/warn/error，默认 info）
/// 与 log.file（可选，空表示仅控制台）。
/// 无依赖模块：忽略初始化上下文。
///
/// @param ctx 初始化上下文（本模块不使用）。
///
/// @return true。
bool CExampleLoggerModule::Initialize(const sc::CResolveContext& /*ctx*/)
{
    common::log::CLogger& logger = common::log::CLogger::Instance();
    std::string level = m_config.GetString("log.level", "info");
    if (level == "trace")
    {
        logger.SetLevel(common::log::LogLevel::kTrace);
    }
    else if (level == "debug")
    {
        logger.SetLevel(common::log::LogLevel::kDebug);
    }
    else if (level == "warn")
    {
        logger.SetLevel(common::log::LogLevel::kWarn);
    }
    else if (level == "error")
    {
        logger.SetLevel(common::log::LogLevel::kError);
    }
    else
    {
        logger.SetLevel(common::log::LogLevel::kInfo);
    }
    std::string logFile = m_config.GetString("log.file", "");
    if (!logFile.empty())
    {
        logger.OpenFile(logFile);
    }
    return true;
}

} // namespace serverexample
