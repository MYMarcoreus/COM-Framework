#include "Module/ServerLoggerModule.h"

#include <string>

#include "Infra/IConfig.h"
#include "Infra/ILogger.h"
#include "Log/Logger.h"

namespace servera {

/// @brief 创建日志模块。
///
/// @param componentManager 组件管理器，用于获取配置与日志组件接口。
CServerLoggerModule::CServerLoggerModule(sc::CComponentManager& componentManager)
    : sc::CModule("logger"), m_componentManager(componentManager)
{
}

/// @brief 销毁日志模块。
CServerLoggerModule::~CServerLoggerModule()
{
}

/// @brief 初始化日志器。
///
/// 通过组件管理器获取 IConfig 读取 log.level，再通过 ILogger 设置日志级别，
/// 演示模块按接口访问基础设施（而非直接使用全局单例）。
///
/// @return true。
bool CServerLoggerModule::Initialize()
{
    // ① 通过 IConfig 读取日志级别配置
    int level = static_cast<int>(common::LogLevel::kInfo);
    sc::IUnknown* configObject = m_componentManager.GetComponent(sc::IID_IConfig());
    if (configObject != nullptr)
    {
        void* raw = nullptr;
        if (configObject->QueryInterface(sc::IID_IConfig(), &raw))
        {
            sc::IConfig* config = static_cast<sc::IConfig*>(raw);
            std::string levelStr = config->GetString("log.level", "info");
            if (levelStr == "trace")
            {
                level = static_cast<int>(common::LogLevel::kTrace);
            }
            else if (levelStr == "debug")
            {
                level = static_cast<int>(common::LogLevel::kDebug);
            }
            else if (levelStr == "warn")
            {
                level = static_cast<int>(common::LogLevel::kWarn);
            }
            else if (levelStr == "error")
            {
                level = static_cast<int>(common::LogLevel::kError);
            }
            else
            {
                level = static_cast<int>(common::LogLevel::kInfo);
            }
        }
    }

    // ② 通过 ILogger 组件接口设置级别并输出日志
    sc::IUnknown* loggerObject = m_componentManager.GetComponent(sc::IID_ILogger());
    if (loggerObject != nullptr)
    {
        void* raw = nullptr;
        if (loggerObject->QueryInterface(sc::IID_ILogger(), &raw))
        {
            sc::ILogger* logger = static_cast<sc::ILogger*>(raw);
            logger->SetLevel(level);
            logger->Info("ServerA 日志模块已通过 ILogger 接口初始化");
        }
    }

    // ③ 启用文件输出
    common::CLogger::Instance().OpenFile("servera.log");
    return true;
}

} // namespace servera
