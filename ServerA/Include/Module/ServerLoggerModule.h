#pragma once

#include "Component/ComponentManager.h"
#include "Module/Module.h"

namespace servera {

/// @brief 日志模块。
///
/// 通过组件管理器按接口（IConfig / ILogger）读取日志配置并初始化日志器，
/// 展示 ServerCore 组件化适配层在真实服务器中的使用。
/// 模块名 "logger"，应在依赖日志的模块之前注册。
class CServerLoggerModule : public sc::CModule
{
public:
    explicit CServerLoggerModule(sc::CComponentManager& componentManager);

    virtual ~CServerLoggerModule();

    // 初始化日志器。
    bool Initialize() override;

private:
    sc::CComponentManager& m_componentManager;
};

} // namespace servera
