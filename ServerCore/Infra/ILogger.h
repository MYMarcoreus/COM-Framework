#pragma once

#include <cstdint>
#include <string>

#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"

namespace sc {

/// @brief 日志接口（模块化适配 common::CLogger）。
///
/// 使模块通过模块管理器按接口获取日志能力，而非直接访问全局单例。
SC_INTERFACE(ILogger, "sc::ILogger", "7f70d36c-e774-49c0-9f0e-0d59b5c0adf8")
{
public:
    virtual ~ILogger() {}

    // 设置日志级别（取值对应 common::LogLevel）。
    virtual void SetLevel(int level) = 0;

    // 启用文件输出。
    virtual bool OpenFile(const std::string& path) = 0;

    // 设置单个日志文件最大字节数（0 表示不滚动）。
    virtual void SetMaxFileSize(std::uint64_t nMaxBytes) = 0;

    // 跟踪日志。
    virtual void Trace(const std::string& message) = 0;

    // 调试日志。
    virtual void Debug(const std::string& message) = 0;

    // 信息日志。
    virtual void Info(const std::string& message) = 0;

    // 警告日志。
    virtual void Warn(const std::string& message) = 0;

    // 错误日志。
    virtual void Error(const std::string& message) = 0;
};

} // namespace sc
