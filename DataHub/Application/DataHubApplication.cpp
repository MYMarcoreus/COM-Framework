#include "Application/DataHubApplication.h"

#include <algorithm>

#include "Log/Logger.h"
#include "Module/Http/HttpServerModule.h"
#include "Module/Storage/DataStoreModule.h"

namespace datahub {

// 配置小工具：负值/非法按 0 处理。
namespace {
// MB → 字节（仅正数）。
std::uint64_t MbToBytes(int nMb)
{
    return nMb > 0 ? static_cast<std::uint64_t>(nMb) * 1024ULL * 1024ULL : 0ULL;
}
// 条数（仅正数）。
std::size_t ToCount(int nVal)
{
    return nVal > 0 ? static_cast<std::size_t>(nVal) : 0u;
}
}  // namespace

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

    // ② 数据存储模块（按接口注册，供 HTTP 服务模块按接口解析）。
    //    容量策略来自 [store] 配置（0 = 不限制）：max_items / max_total_mb / max_item_mb。
    int nStoreMaxItems = m_config.GetInt("store.max_items", 0);
    std::uint64_t nStoreTotal = MbToBytes(m_config.GetInt("store.max_total_mb", 0));
    std::uint64_t nStoreItem = MbToBytes(m_config.GetInt("store.max_item_mb", 0));
    if (!m_moduleManager.RegisterModule(sc::IID_IDataStore(),
                                        new CDataStoreModule(ToCount(nStoreMaxItems), nStoreTotal, nStoreItem)))
    {
        return false;
    }

    // ③ HTTP 数据传输服务模块（基于 Sogou Workflow）
    //    前端页面为独立资源文件：构建时部署到用户目录 ~/.datahub/。
    //    配置 [web] dir 指定静态资源目录；留空则使用默认用户目录 ~/.datahub。
    //    单次上传/请求体上限来自 [http] max_body_bytes（缺省 32MB，0 表示不限制）。
    std::string strWebDir = m_config.GetString("web.dir", "");
    int nMaxBodyBytes = m_config.GetInt("http.max_body_bytes", 33554432);
    if (!m_moduleManager.RegisterModule(
            new CHttpServerModule(m_nPort, strWebDir,
                                  nMaxBodyBytes > 0 ? static_cast<std::uint64_t>(nMaxBodyBytes) : 0ULL)))
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
