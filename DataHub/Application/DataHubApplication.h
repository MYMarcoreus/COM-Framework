#pragma once

#include <cstdint>
#include <string>

#include "Application/MyApplication.h"
#include "Config/Config.h"

namespace datahub {

/// @brief DataHub 服务器应用程序（基于 ServerCore 骨架 + Sogou Workflow HTTP）。
///
/// 复用 ServerCore 的 CMyApplication 生命周期与模块模型：
///   - RegisterModules：基类默认装配（IConfig/ILogger/IMetrics）
///     → 数据存储模块（IDataStore）→ HTTP 服务模块（IHttpService）。
///   - HTTP 由 Workflow（WFHttpServer）提供，封装为 ServerCore 模块。
class CDataHubApplication : public sc::CMyApplication
{
public:
    // @param port 监听端口；0 表示从配置文件读取。
    explicit CDataHubApplication(std::uint16_t port);

    virtual ~CDataHubApplication();

protected:
    bool RegisterModules() override;
    bool OnInitialize() override;
    bool OnStart() override;
    void OnShutdown() override;

private:
    std::uint16_t m_nPort;
    common::config::CConfig m_config;
};

} // namespace datahub
