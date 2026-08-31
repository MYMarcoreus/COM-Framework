#include "Module/TemplateLoggerModule.h"

#include <string>

#include "Infra/IConfig.h"
#include "Infra/ILogger.h"
#include "Log/Logger.h"
#include "Module/ResolveContext.h"

namespace servertemplate {

/// @brief 创建日志模块。
CTemplateLoggerModule::CTemplateLoggerModule()
    : sc::CModule("logger")
{
}

/// @brief 销毁日志模块。
CTemplateLoggerModule::~CTemplateLoggerModule()
{
}

/// @brief 模块启动（日志器由全局单例管理，无独立启动资源）。
bool CTemplateLoggerModule::Start()
{
    return true;
}

/// @brief 模块停止（全局日志单例持续运行，无需处理）。
void CTemplateLoggerModule::Stop()
{
}

/// @brief 模块关闭（全局日志单例由全局管理，无需处理）。
void CTemplateLoggerModule::Shutdown()
{
}

/// @brief 初始化日志器。
///
/// 通过初始化上下文获取 IConfig 读取 log.level，再通过 ILogger 设置日志级别，
/// 演示模块按接口访问基础设施（而非直接使用全局单例）。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true。
bool CTemplateLoggerModule::Initialize(const sc::CResolveContext& ctx)
{
    // ① 通过 IConfig 读取日志级别配置
    int level = static_cast<int>(common::log::LogLevel::kInfo);
    sc::IConfig* config = ctx.Resolve<sc::IConfig>();
    if (config != nullptr)
    {
        std::string levelStr = config->GetString("log.level", "info");
        if (levelStr == "trace")
        {
            level = static_cast<int>(common::log::LogLevel::kTrace);
        }
        else if (levelStr == "debug")
        {
            level = static_cast<int>(common::log::LogLevel::kDebug);
        }
        else if (levelStr == "warn")
        {
            level = static_cast<int>(common::log::LogLevel::kWarn);
        }
        else if (levelStr == "error")
        {
            level = static_cast<int>(common::log::LogLevel::kError);
        }
        else
        {
            level = static_cast<int>(common::log::LogLevel::kInfo);
        }
    }

    // ② 通过 ILogger 模块接口设置级别、启用文件输出并输出日志
    sc::ILogger* logger = ctx.Resolve<sc::ILogger>();
    if (logger != nullptr)
    {
        logger->SetLevel(level);
        logger->OpenFile("template.log");
        logger->Info("ServerTemplate 日志模块已通过 ILogger 接口初始化");
    }
    return true;
}

} // namespace servertemplate
