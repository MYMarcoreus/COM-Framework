#include "Module/Http/TenantsController.h"

#include <sstream>
#include <string>

#include "Framework/HttpRouter.h"
#include "Framework/HttpText.h"

namespace datahub {

using sc::CTenant;
using sc::ITenantService;

/// @brief 创建租户控制器。
CTenantsController::CTenantsController(ITenantService* pTenants) : m_pTenants(pTenants) {}

/// @brief 注册租户资源路由。
void CTenantsController::RegisterRoutes(web::CHttpRouter& router)
{
    router.Register({"POST", "/api/tenant",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleCreate(req, resp); }});
    router.Register({"GET", "/api/tenant/info",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleInfo(req, resp); }});
}

/// @brief 创建租户：POST /api/tenant —— 名称为请求体（workflow 拒零长 body，故恒非空）。
bool CTenantsController::HandleCreate(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pTenants == nullptr)
    {
        resp.WriteJson("{\"error\":\"tenant service unavailable\"}", "500");
        return true;
    }
    // 名称取请求体并去首尾空白。
    std::string strName = req.Body();
    std::string::size_type nStart = strName.find_first_not_of(" \t\r\n");
    std::string::size_type nEnd = strName.find_last_not_of(" \t\r\n");
    if (nStart == std::string::npos)
    {
        strName.clear();
    }
    else
    {
        strName = strName.substr(nStart, nEnd - nStart + 1);
    }
    if (strName.size() > 48)
    {
        strName = strName.substr(0, 48);
    }
    CTenant tenant;
    if (!m_pTenants->CreateTenant(strName, tenant))
    {
        resp.WriteJson("{\"error\":\"create failed\"}", "500");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"code\":" << web::CHttpText::JsonString(tenant.strCode)
        << ",\"name\":" << web::CHttpText::JsonString(tenant.strName) << "}";
    resp.WriteJson(oss.str());
    return true;
}

/// @brief 查询租户：GET /api/tenant/info?code=xxx —— 供"凭码加入"前校验存在性。
bool CTenantsController::HandleInfo(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pTenants == nullptr)
    {
        resp.WriteJson("{\"error\":\"tenant service unavailable\"}", "500");
        return true;
    }
    std::string strCode = req.QueryParam("code");
    if (strCode.empty())
    {
        resp.WriteJson("{\"error\":\"missing code\"}", "400");
        return true;
    }
    CTenant tenant;
    if (!m_pTenants->FindTenant(strCode, tenant))
    {
        resp.WriteJson("{\"error\":\"tenant not found\"}", "404");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"code\":" << web::CHttpText::JsonString(tenant.strCode)
        << ",\"name\":" << web::CHttpText::JsonString(tenant.strName) << ",\"created\":" << tenant.nCreateMs << "}";
    resp.WriteJson(oss.str());
    return true;
}

}  // namespace datahub
