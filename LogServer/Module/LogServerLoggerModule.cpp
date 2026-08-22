#include "Module/LogServerLoggerModule.h"

#include <string>

#include "Infra/IConfig.h"
#include "Infra/ILogger.h"
#include "Log/Logger.h"
#include "Module/ResolveContext.h"

namespace logserver {

/// @brief 创建日志模块。
CLogServerLoggerModule::CLogServerLoggerModule()
    : sc::CModule("logger")
{
}

/// @brief 销毁日志模块。
CLogServerLoggerModule::~CLogServerLoggerModule()
{
}

/// @brief 模块启动（日志器由全局单例管理，无独立启动资源）。
bool CLogServerLoggerModule::Start()
{
    return true;
}

/// @brief 模块停止（全局日志单例持续运行，无需处理）。
void CLogServerLoggerModule::Stop()
{
}

/// @brief 模块关闭（全局日志单例由全局管理，无需处理）。
void CLogServerLoggerModule::Shutdown()
{
}

/// @brief 初始化日志器。
///
/// 通过初始化上下文获取 IConfig 读取 log.level 与 log.file，
/// 再通过 ILogger 设置日志级别并启用文件输出。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true。
bool CLogServerLoggerModule::Initialize(const sc::CResolveContext& ctx)
{
    // ① 通过 IConfig 读取日志级别配置
    int nLevel = static_cast<int>(common::LogLevel::kInfo);
    std::string strFilePath = "logserver.log";
    sc::IConfig* pConfig = ctx.Resolve<sc::IConfig>();
    if (pConfig != nullptr)
    {
        std::string strLevel = pConfig->GetString("log.level", "info");
        if (strLevel == "trace")
        {
            nLevel = static_cast<int>(common::LogLevel::kTrace);
        }
        else if (strLevel == "debug")
        {
            nLevel = static_cast<int>(common::LogLevel::kDebug);
        }
        else if (strLevel == "warn")
        {
            nLevel = static_cast<int>(common::LogLevel::kWarn);
        }
        else if (strLevel == "error")
        {
            nLevel = static_cast<int>(common::LogLevel::kError);
        }
        strFilePath = pConfig->GetString("log.file", "logserver.log");
    }

    // ② 通过 ILogger 模块接口设置级别、启用文件输出并输出日志
    sc::ILogger* pLogger = ctx.Resolve<sc::ILogger>();
    if (pLogger != nullptr)
    {
        pLogger->SetLevel(nLevel);
        pLogger->OpenFile(strFilePath);
        pLogger->Info("LogServer 日志模块已通过 ILogger 接口初始化");
    }
    return true;
}

} // namespace logserver
