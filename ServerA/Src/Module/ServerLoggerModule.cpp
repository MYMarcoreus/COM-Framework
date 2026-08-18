#include "Module/ServerLoggerModule.h"

#include "Log/Logger.h"

namespace servera {

/// @brief 创建日志模块。
ServerLoggerModule::ServerLoggerModule() : sc::Module("logger")
{
}

/// @brief 销毁日志模块。
ServerLoggerModule::~ServerLoggerModule()
{
}

/// @brief 初始化日志器。
///
/// 配置为 Info 级别，控制台 + 文件（servera.log）。
///
/// @return true。
bool ServerLoggerModule::Initialize()
{
    common::Logger& logger = common::Logger::Instance();
    logger.SetLevel(common::LogLevel::kInfo);
    logger.OpenFile("servera.log");
    return true;
}

} // namespace servera
