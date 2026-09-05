#pragma once

#include <cstdint>
#include <string>

#include "Application/MyApplication.h"
#include "Config/Config.h"

namespace datahubadmin {

/// @brief DataHubAdmin 管理控制面应用程序（独立服务器 exe）。
///
/// 职责：为服务器运维提供租户管理网页，并把 /api/admin/* 代理到 DataHub
/// 数据面（数据面只在本机回环 + 令牌下开放管理接口）。本进程不持有业务数据，
/// 是典型的"控制面与数据面分离"中的控制面。
///
///   DataHubAdmin(8899)  --代理 /api/admin/*-->  DataHub(8888, loopback+token)
///
/// 复用 ServerCore 的 CMyApplication 生命周期：RegisterModules 装配
/// IConfig/ILogger/IMetrics + 管理控制台 HTTP 服务模块（CAdminConsoleModule）。
class CDataHubAdminApplication : public sc::CMyApplication
{
   public:
    // @param port 控制台监听端口；0 表示从配置文件读取。
    explicit CDataHubAdminApplication(std::uint16_t port);

    virtual ~CDataHubAdminApplication();

   protected:
    bool RegisterModules() override;
    bool OnInitialize() override;
    bool OnStart() override;
    void OnShutdown() override;

   private:
    std::uint16_t m_nPort;
    common::config::CConfig m_config;
};

}  // namespace datahubadmin
