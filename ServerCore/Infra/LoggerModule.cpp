#include "Infra/LoggerModule.h"

#include "Module/InterfaceMap.h"
#include <string>

#include "Log/Logger.h"

namespace sc {

// 接口映射表：暴露本类实现的接口（查表驱动 QueryInterface）。
SC_DEFINE_INTERFACE_MAP(CLoggerModule, CModule, ILogger)

/// @brief 创建日志模块。
CLoggerModule::CLoggerModule() : CModule("logger")
{
}

/// @brief 销毁日志模块。
CLoggerModule::~CLoggerModule()
{
}

/// @brief 初始化模块（代理全局日志单例，无配置依赖）。
bool CLoggerModule::Initialize(const CResolveContext& /*ctx*/)
{
    return true;
}

/// @brief 模块启动（无独立启动资源）。
bool CLoggerModule::Start()
{
    return true;
}

/// @brief 模块停止（日志单例持续运行，无需处理）。
void CLoggerModule::Stop()
{
}

/// @brief 模块关闭（日志单例由全局管理，无需处理）。
void CLoggerModule::Shutdown()
{
}

/// @brief 设置日志级别。
///
/// @param level 对应 common::LogLevel 的枚举值。
void CLoggerModule::SetLevel(int level)
{
    common::CLogger::Instance().SetLevel(static_cast<common::LogLevel>(level));
}

/// @brief 启用文件输出。
bool CLoggerModule::OpenFile(const std::string& path)
{
    return common::CLogger::Instance().OpenFile(path);
}

/// @brief 设置单个日志文件最大字节数。
///
/// @param nMaxBytes 文件上限；0 表示不滚动。
void CLoggerModule::SetMaxFileSize(std::uint64_t nMaxBytes)
{
    common::CLogger::Instance().SetMaxFileSize(nMaxBytes);
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

} // namespace sc
