#pragma once

#include "Module/Module.h"

namespace servera {

/// @brief 日志模块。
///
/// 初始化时配置全局日志器（控制台 + 文件）。
/// 模块名 "logger"，应在依赖日志的模块之前注册。
class ServerLoggerModule : public sc::Module
{
public:
    ServerLoggerModule();

    virtual ~ServerLoggerModule();

    // 初始化日志器。
    bool Initialize() override;

private:
};

} // namespace servera
