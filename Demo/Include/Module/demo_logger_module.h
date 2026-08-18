#pragma once

#include "Config/config.h"
#include "Module/module.h"

namespace demo {

/// @brief 日志模块。
///
/// 初始化时根据配置（log.level / log.file）配置全局日志器。
/// 模块名 "logger"，应在依赖日志的模块之前注册。
class CDemoLoggerModule : public sc::CModule
{
public:
    explicit CDemoLoggerModule(const common::CConfig& config);

    virtual ~CDemoLoggerModule();

    // 根据配置初始化日志器。
    bool Initialize() override;

private:
    const common::CConfig& config_;
};

} // namespace demo
