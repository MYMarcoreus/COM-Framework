#pragma once

#include "Module/ModuleManager.h"
#include "Module/Module.h"

namespace logserver {

/// @brief 日志模块。
///
/// 通过初始化上下文按接口（IConfig / ILogger）读取日志配置并初始化日志器，
/// 复用 ServerCore 模块化适配层。模块名 "logger"，
/// 应在依赖日志的模块之前注册。
class CLogServerLoggerModule : public sc::CModule
{
public:
    CLogServerLoggerModule();

    virtual ~CLogServerLoggerModule();

    // 初始化日志器。
    bool Initialize(const sc::CResolveContext& ctx) override;

    // 生命周期：日志器由全局单例管理，无独立资源。
    bool Start() override;
    void Stop() override;
    void Shutdown() override;
};

} // namespace logserver
