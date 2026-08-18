#include "Infra/i_logger.h"

#include <string>

#include "Log/logger.h"

namespace sc {

/// @brief 创建日志组件。
CLoggerComponent::CLoggerComponent()
{
}

/// @brief 销毁日志组件。
CLoggerComponent::~CLoggerComponent()
{
}

/// @brief 设置日志级别。
///
/// @param level 对应 common::LogLevel 的枚举值。
void CLoggerComponent::SetLevel(int level)
{
    common::CLogger::Instance().SetLevel(static_cast<common::LogLevel>(level));
}

/// @brief 跟踪日志。
void CLoggerComponent::Trace(const std::string& message)
{
    common::CLogger::Instance().Trace(message);
}

/// @brief 调试日志。
void CLoggerComponent::Debug(const std::string& message)
{
    common::CLogger::Instance().Debug(message);
}

/// @brief 信息日志。
void CLoggerComponent::Info(const std::string& message)
{
    common::CLogger::Instance().Info(message);
}

/// @brief 警告日志。
void CLoggerComponent::Warn(const std::string& message)
{
    common::CLogger::Instance().Warn(message);
}

/// @brief 错误日志。
void CLoggerComponent::Error(const std::string& message)
{
    common::CLogger::Instance().Error(message);
}

/// @brief 接口查询实现。
bool CLoggerComponent::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_ILogger()))
    {
        *ppv = static_cast<ILogger*>(this);
        return true;
    }
    return CComponent::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
