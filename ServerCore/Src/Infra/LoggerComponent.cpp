#include "Infra/ILogger.h"

#include <string>

#include "Log/Logger.h"

namespace sc {

/// @brief 创建日志组件。
LoggerComponent::LoggerComponent()
{
}

/// @brief 销毁日志组件。
LoggerComponent::~LoggerComponent()
{
}

/// @brief 设置日志级别。
///
/// @param level 对应 common::LogLevel 的枚举值。
void LoggerComponent::SetLevel(int level)
{
    common::Logger::Instance().SetLevel(static_cast<common::LogLevel>(level));
}

/// @brief 跟踪日志。
void LoggerComponent::Trace(const std::string& message)
{
    common::Logger::Instance().Trace(message);
}

/// @brief 调试日志。
void LoggerComponent::Debug(const std::string& message)
{
    common::Logger::Instance().Debug(message);
}

/// @brief 信息日志。
void LoggerComponent::Info(const std::string& message)
{
    common::Logger::Instance().Info(message);
}

/// @brief 警告日志。
void LoggerComponent::Warn(const std::string& message)
{
    common::Logger::Instance().Warn(message);
}

/// @brief 错误日志。
void LoggerComponent::Error(const std::string& message)
{
    common::Logger::Instance().Error(message);
}

/// @brief 接口查询实现。
bool LoggerComponent::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_ILogger()))
    {
        *ppv = static_cast<ILogger*>(this);
        return true;
    }
    return Component::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
