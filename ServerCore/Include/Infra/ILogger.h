#pragma once

#include <string>

#include "Component/Component.h"

namespace sc {

/// @brief 获取 ILogger 接口标识。
inline const InterfaceId& IID_ILogger()
{
    static const InterfaceId iid = "sc::ILogger";
    return iid;
}

/// @brief 日志接口（组件化适配 common::Logger）。
///
/// 使模块通过组件管理器按接口获取日志能力，而非直接访问全局单例。
class ILogger : public virtual IUnknown
{
public:
    virtual ~ILogger() {}

    // 设置日志级别（取值对应 common::LogLevel）。
    virtual void SetLevel(int level) = 0;

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

/// @brief 日志组件。
///
/// 内部代理 common::Logger 全局单例。
class LoggerComponent : public Component, public ILogger
{
public:
    LoggerComponent();

    virtual ~LoggerComponent();

    void SetLevel(int level) override;
    void Trace(const std::string& message) override;
    void Debug(const std::string& message) override;
    void Info(const std::string& message) override;
    void Warn(const std::string& message) override;
    void Error(const std::string& message) override;

protected:
    // 接口查询实现。
    bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv) override;
};

} // namespace sc
