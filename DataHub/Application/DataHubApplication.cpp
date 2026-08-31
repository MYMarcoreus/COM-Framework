#include "Application/DataHubApplication.h"

#include "Log/Logger.h"
#include "Module/DataStoreModule.h"
#include "Module/HttpServerModule.h"

namespace datahub {

/// @brief 创建 DataHub 应用程序。
///
/// @param port 监听端口；0 表示从配置文件读取。
CDataHubApplication::CDataHubApplication(std::uint16_t port) : m_nPort(port)
{
    // 加载配置文件（可选，best-effort）。
    m_config.LoadFile("datahub.ini");
    if (m_nPort == 0)
    {
        int configPort = m_config.GetInt("server.port", 8888);
        if (configPort > 0 && configPort <= 65535)
        {
            m_nPort = static_cast<std::uint16_t>(configPort);
        }
        else
        {
            m_nPort = 8888;
        }
    }
}

/// @brief 销毁 DataHub 应用程序。
CDataHubApplication::~CDataHubApplication() {}

/// @brief 注册模块。
///
/// 注册顺序即初始化/启动顺序：
///   基类默认装配 → 数据存储模块（IDataStore）→ HTTP 服务模块（IHttpService）。
/// HTTP 服务模块声明依赖 IDataStore，由 CModuleManager 拓扑排序保证先就绪。
///
/// @return true 全部注册成功；false 注册失败。
bool CDataHubApplication::RegisterModules()
{
    // ① 基类默认装配（IConfig + ILogger + IMetrics）
    if (!CMyApplication::RegisterModules())
    {
        return false;
    }

    // ② 数据存储模块（按接口注册，供 HTTP 服务模块按接口解析）
    if (!m_moduleManager.RegisterModule(sc::IID_IDataStore(), new CDataStoreModule()))
    {
        return false;
    }

    // ③ HTTP 数据传输服务模块（基于 Sogou Workflow）
    //    前端页面为独立资源文件，路径由配置 [web] index 指定（默认 Web/index.html）。
    std::string strIndex = m_config.GetString("web.index", "Web/index.html");
    if (!m_moduleManager.RegisterModule(new CHttpServerModule(m_nPort, strIndex)))
    {
        return false;
    }
    return true;
}

/// @brief 初始化完成钩子。
///
/// @return true。
bool CDataHubApplication::OnInitialize()
{
    return true;
}

/// @brief 启动完成钩子。
///
/// @return true。
bool CDataHubApplication::OnStart()
{
    common::log::CLogger::Instance().Info("[DataHub] 数据传输服务已就绪（ServerCore 骨架 + Workflow HTTP）");
    return true;
}

/// @brief 关闭钩子。
void CDataHubApplication::OnShutdown() {}

}  // namespace datahub
