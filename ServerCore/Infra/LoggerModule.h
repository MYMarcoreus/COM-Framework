#pragma once

#include <cstdint>
#include <string>

#include "Infra/ILogger.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"

namespace sc {

/// @brief 日志模块。
///
/// 内部代理 common::CLogger 全局单例。
class CLoggerModule : public CModule, public ILogger
{
public:
    CLoggerModule();

    virtual ~CLoggerModule();

    // 生命周期：代理全局日志单例，无独立生命周期资源。
    bool Initialize(const CResolveContext& ctx) override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    void SetLevel(int level) override;
    bool OpenFile(const std::string& path) override;
    void SetMaxFileSize(std::uint64_t nMaxBytes) override;
    void Trace(const std::string& message) override;
    void Debug(const std::string& message) override;
    void Info(const std::string& message) override;
    void Warn(const std::string& message) override;
    void Error(const std::string& message) override;

    SC_DECLARE_INTERFACE_MAP();
};

} // namespace sc
