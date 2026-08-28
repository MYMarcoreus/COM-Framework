#pragma once

#include "Config/Config.h"
#include "Module/Module.h"

namespace demo {

/// @brief 日志模块。
///
/// 初始化时根据配置（log.level / log.file）配置全局日志器。
/// 模块名 "logger"，应在依赖日志的模块之前注册。
class CDemoLoggerModule : public sc::CModule
{
public:
    explicit CDemoLoggerModule(const common::config::CConfig& config);

    virtual ~CDemoLoggerModule();

    // 根据配置初始化日志器。
    bool Initialize(const sc::CResolveContext& ctx) override;

    // 生命周期：日志器由全局单例管理，无独立资源。
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

private:
    const common::config::CConfig& m_config;
};

} // namespace demo
