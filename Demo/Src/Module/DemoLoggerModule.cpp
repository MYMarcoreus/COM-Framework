#include "Module/DemoLoggerModule.h"

#include <string>

#include "Log/Logger.h"

namespace demo {

/// @brief 创建日志模块。
///
/// @param config 应用配置，用于读取日志级别与文件。
CDemoLoggerModule::CDemoLoggerModule(const common::CConfig& config)
    : sc::CModule("logger"), m_config(config)
{
}

/// @brief 销毁日志模块。
CDemoLoggerModule::~CDemoLoggerModule()
{
}

/// @brief 根据配置初始化日志器。
///
/// 读取 log.level（trace/debug/info/warn/error，默认 info）
/// 与 log.file（可选，空表示仅控制台）。
///
/// @return true。
bool CDemoLoggerModule::Initialize()
{
    common::CLogger& logger = common::CLogger::Instance();
    std::string level = m_config.GetString("log.level", "info");
    if (level == "trace")
    {
        logger.SetLevel(common::LogLevel::kTrace);
    }
    else if (level == "debug")
    {
        logger.SetLevel(common::LogLevel::kDebug);
    }
    else if (level == "warn")
    {
        logger.SetLevel(common::LogLevel::kWarn);
    }
    else if (level == "error")
    {
        logger.SetLevel(common::LogLevel::kError);
    }
    else
    {
        logger.SetLevel(common::LogLevel::kInfo);
    }
    std::string logFile = m_config.GetString("log.file", "");
    if (!logFile.empty())
    {
        logger.OpenFile(logFile);
    }
    return true;
}

} // namespace demo
