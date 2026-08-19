#include "Infra/ILogger.h"

#include <string>

#include "Log/Logger.h"

namespace sc {

/// @brief 创建日志模块。
CLoggerModule::CLoggerModule() : CModule("logger")
{
}

/// @brief 销毁日志模块。
CLoggerModule::~CLoggerModule()
{
}

/// @brief 设置日志级别。
///
/// @param level 对应 common::LogLevel 的枚举值。
void CLoggerModule::SetLevel(int level)
{
    common::CLogger::Instance().SetLevel(static_cast<common::LogLevel>(level));
}

/// @brief 跟踪日志。
void CLoggerModule::Trace(const std::string& message)
{
    common::CLogger::Instance().Trace(message);
}

/// @brief 调试日志。
void CLoggerModule::Debug(const std::string& message)
{
    common::CLogger::Instance().Debug(message);
}

/// @brief 信息日志。
void CLoggerModule::Info(const std::string& message)
{
    common::CLogger::Instance().Info(message);
}

/// @brief 警告日志。
void CLoggerModule::Warn(const std::string& message)
{
    common::CLogger::Instance().Warn(message);
}

/// @brief 错误日志。
void CLoggerModule::Error(const std::string& message)
{
    common::CLogger::Instance().Error(message);
}

/// @brief 接口查询实现。
bool CLoggerModule::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_ILogger()))
    {
        *ppv = static_cast<ILogger*>(this);
        return true;
    }
    return CModule::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
