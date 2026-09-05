#include "Application/DataHubAdminApplication.h"

#include <string>

#include "Log/Logger.h"
#include "Module/AdminConsoleModule.h"

namespace datahubadmin {

/// @param port 监听端口；0 表示从配置文件读取。
CDataHubAdminApplication::CDataHubAdminApplication(std::uint16_t port) : m_nPort(port)
{
    // 加载配置文件（可选，best-effort）。
    m_config.LoadFile("datahub-admin.ini");
    if (m_nPort == 0)
    {
        int configPort = m_config.GetInt("server.port", 8899);
        if (configPort > 0 && configPort <= 65535)
        {
            m_nPort = static_cast<std::uint16_t>(configPort);
        }
        else
        {
            m_nPort = 8899;
        }
    }
}

CDataHubAdminApplication::~CDataHubAdminApplication() {}

/// @brief 注册模块。
///
/// 基类默认装配（IConfig/ILogger/IMetrics）→ 管理控制台服务模块
/// （CAdminConsoleModule：管理网页 + /api/admin 代理到 DataHub）。
bool CDataHubAdminApplication::RegisterModules()
{
    if (!CMyApplication::RegisterModules())
    {
        return false;
    }

    // 上游（DataHub 数据面）与本机访问令牌。
    const std::string strUpstreamBase = m_config.GetString("upstream.base", "http://127.0.0.1:8888");
    const std::string strUpstreamToken = m_config.GetString("upstream.token", "");
    const std::string strWebDir = m_config.GetString("web.dir", "");

    if (!m_moduleManager.RegisterModule(new CAdminConsoleModule(m_nPort, strUpstreamBase, strUpstreamToken, strWebDir)))
    {
        return false;
    }
    return true;
}

bool CDataHubAdminApplication::OnInitialize()
{
    return true;
}

bool CDataHubAdminApplication::OnStart()
{
    common::log::CLogger::Instance().Info("[DataHubAdmin] 管理控制面已就绪（ServerCore 骨架 + Workflow HTTP）");
    return true;
}

void CDataHubAdminApplication::OnShutdown() {}

}  // namespace datahubadmin
