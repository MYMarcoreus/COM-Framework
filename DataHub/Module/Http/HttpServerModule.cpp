#include "Module/Http/HttpServerModule.h"

#include <cstdlib>
#include <exception>
#include <string>

#include "Log/Logger.h"
#include "Module/Http/HttpHandlers.h"
#include "Module/Http/MemberService.h"
#include "Module/Http/WebPageController.h"
#include "Module/InterfaceMap.h"
#include "Module/ResolveContext.h"
#include "workflow/HttpMessage.h"

namespace datahub {

/// @brief 创建 HTTP 服务模块。
CHttpServerModule::CHttpServerModule(std::uint16_t nPort, const std::string& strWebDir)
    : CHttpServerModule(nPort, strWebDir, kDefaultMaxBodyBytes)
{}

/// @brief 创建 HTTP 服务模块（带单次上传/请求体上限）。
CHttpServerModule::CHttpServerModule(std::uint16_t nPort, const std::string& strWebDir, std::uint64_t nMaxBodyBytes)
    : sc::CModule("http"),
      m_nPort(nPort),
      m_strWebDir(strWebDir),
      m_nMaxBodyBytes(nMaxBodyBytes),
      m_server([this](WFHttpTask* pTask) { OnRequest(pTask); }),
      m_bStarted(false)
{
    // 依赖接口模块：生命周期拓扑排序保证其先初始化 / 启动。
    AddDependency(sc::IID_IDataStore());
    AddDependency(sc::IID_ITenantService());

    // 前端目录归一化：空串回退到默认用户目录 $HOME/.datahub（仅解析一次）。
    // 后续由 CWebPageController 使用该目录加载前端资源。
    if (m_strWebDir.empty())
    {
        const char* szHome = ::getenv("HOME");
        if (szHome != nullptr)
        {
            m_strWebDir = std::string(szHome) + "/.datahub";
        }
    }
}

/// @brief 销毁 HTTP 服务模块。
CHttpServerModule::~CHttpServerModule()
{
    Stop();
}

/// @brief 从初始化上下文解析数据存储接口，组装业务层并注册路由。
bool CHttpServerModule::Initialize(const sc::CResolveContext& ctx)
{
    m_pStore.Reset(ctx.Resolve<IDataStore>());
    if (m_pStore == nullptr)
    {
        return false;
    }

    // 租户注册表（解析每个请求的 X-Space → CTenant）。
    m_pTenants.Reset(ctx.Resolve<sc::ITenantService>());
    if (m_pTenants == nullptr)
    {
        return false;
    }

    // 可观测性：可选依赖 IMetrics（未装配时不上报，不影响启动）。
    m_pMetrics.Reset(ctx.Resolve<sc::IMetrics>());

    // 业务层实例（依赖注入）。
    m_pMembers = std::unique_ptr<CMemberService>(new CMemberService());
    m_pHandlers =
        std::unique_ptr<CHttpHandlers>(new CHttpHandlers(m_pStore.Get(), m_pMembers.get(), m_pTenants.Get(), m_nMaxBodyBytes));
    m_pPages = std::unique_ptr<CWebPageController>(new CWebPageController(m_strWebDir));

    // 前端资源一次性读入内存（缺失时由控制器回 503 / 404）。
    if (m_pPages->Load())
    {
        common::log::CLogger::Instance().Info("[DataHub] 前端资源已加载: " + m_strWebDir);
    }

    // 注册路由（框架层 CHttpRouter）。
    // —— 业务 API：由业务控制器（CHttpHandlers）自注册。
    m_pHandlers->RegisterRoutes(m_router);
    // —— 页面 / 静态资源：由页面控制器（CWebPageController）自注册。
    m_pPages->RegisterRoutes(m_router);
    return true;
}

/// @brief 启动 HTTP 服务。
bool CHttpServerModule::Start()
{
    if (m_bStarted)
    {
        return true;
    }
    if (m_server.start(m_nPort) == 0)
    {
        m_bStarted = true;
        common::log::CLogger::Instance().Info("[DataHub] HTTP 服务已启动，监听端口 " + std::to_string(m_nPort));
        return true;
    }
    return false;
}

/// @brief 停止 HTTP 服务。
void CHttpServerModule::Stop()
{
    if (m_bStarted)
    {
        m_server.stop();
        m_bStarted = false;
        common::log::CLogger::Instance().Info("[DataHub] HTTP 服务已停止");
    }
}

void CHttpServerModule::Shutdown()
{
    Stop();
    m_pPages.reset();
    m_pHandlers.reset();
    m_pMembers.reset();
    m_pTenants.Reset();
    m_pStore.Reset();
}

std::uint16_t CHttpServerModule::Port() const
{
    return m_nPort;
}

std::string CHttpServerModule::Status() const
{
    return "http:port=" + std::to_string(m_nPort) + " started=" + (m_bStarted ? "1" : "0");
}

// ----------------------------------------------------------------------------
// 请求处理回调（Workflow 线程池中执行）
// ----------------------------------------------------------------------------
void CHttpServerModule::OnRequest(WFHttpTask* pServerTask)
{
    if (m_pMetrics != nullptr)
    {
        m_pMetrics->Inc("http.requests");
    }

    // 封装请求 / 响应（业务层不直接接触 workflow 类型）。
    web::CHttpRequest req(pServerTask);
    web::CHttpResponse resp(pServerTask);

    // 当前租户（生命周期在本函数栈；经 req.UserData() 供业务读取）。
    sc::CTenant tenant;
    try
    {
        // 1) 租户解析：缺省 X-Space → 公共租户；未知空间码回 404（防止越权到别的租户）。
        std::string strCode = req.Header("X-Space");
        if (strCode.empty() && m_pTenants != nullptr)
        {
            strCode = m_pTenants->DefaultCode();
        }
        const bool bResolved = m_pTenants != nullptr && m_pTenants->FindTenant(strCode, tenant);
        if (!bResolved)
        {
            resp.WriteJson("{\"error\":\"space not found\"}", "404");
        }
        else
        {
            // 2) 挂到请求上下文 → 成员记录（按租户）→ 路由分发。
            req.SetUserData(&tenant);
            if (m_pMembers)
            {
                m_pMembers->Touch(req, tenant);
            }
            if (m_pMetrics != nullptr)
            {
                m_pMetrics->SetGauge("http.members",
                                     static_cast<double>(m_pMembers != nullptr ? m_pMembers->Count(tenant) : 0));
            }
            if (!m_router.Dispatch(req, resp))
            {
                resp.WriteText("Not Found", "404", "text/plain");
            }
        }
    }
    catch (const std::exception& e)
    {
        common::log::CLogger::Instance().Error("[DataHub] 请求处理异常: " + std::string(e.what()));
        resp.WriteText("Internal Server Error", "500", "text/plain");
    }
    catch (...)
    {
        common::log::CLogger::Instance().Error("[DataHub] 请求处理未知异常");
        resp.WriteText("Internal Server Error", "500", "text/plain");
    }

    // 按状态码分布记录（供错误率 / 可用性观测）。
    if (m_pMetrics != nullptr)
    {
        std::string strStatus = resp.StatusCode();
        std::string strClass = "5xx";
        if (!strStatus.empty())
        {
            char cFirst = strStatus[0];
            if (cFirst == '2')
                strClass = "2xx";
            else if (cFirst == '3')
                strClass = "3xx";
            else if (cFirst == '4')
                strClass = "4xx";
        }
        m_pMetrics->Inc("http.status." + strClass);
    }

    // 统一响应头。
    protocol::HttpResponse* pRaw = pServerTask->get_resp();
    pRaw->add_header_pair("Server", "DataHub/1.0");
}

SC_BEGIN_INTERFACE_MAP(CHttpServerModule, sc::CModule)
SC_INTERFACE_ENTRY(IHttpService)
SC_END_INTERFACE_MAP(CHttpServerModule, sc::CModule)

}  // namespace datahub
