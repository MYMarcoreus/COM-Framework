#pragma once

#include "Config/Config.h"
#include "Module/Module.h"

namespace demo {

/// @brief 日志模块。
///
/// 初始化时根据配置（log.level / log.file）配置全局日志器。
/// 模块名 "logger"，应在依赖日志的模块之前注册。
class DemoLoggerModule : public sc::Module
{
public:
    explicit DemoLoggerModule(const common::Config& config);

    virtual ~DemoLoggerModule();

    // 根据配置初始化日志器。
    bool Initialize() override;

private:
    const common::Config& config_;
};

} // namespace demo
