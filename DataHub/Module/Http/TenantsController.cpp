#include "Module/Http/TenantsController.h"

#include <sstream>
#include <string>
#include <vector>

#include "Framework/HttpRouter.h"
#include "Framework/HttpText.h"

namespace datahub {

using sc::CTenant;
using sc::CTenantMember;
using sc::ITenantService;
using sc::TenantRole;

namespace {
// 账号：X-Client-Id（浏览器持久化），缺省退回对端地址。
std::string AccountId(web::CHttpRequest& req)
{
    std::string strAccount = req.Header("X-Client-Id");
    return strAccount.empty() ? req.Peer() : strAccount;
}
// 去掉字符串首尾空白。
std::string Trim(const std::string& str)
{
    std::string::size_type nStart = str.find_first_not_of(" \t\r\n");
    if (nStart == std::string::npos)
    {
        return std::string();
    }
    std::string::size_type nEnd = str.find_last_not_of(" \t\r\n");
    return str.substr(nStart, nEnd - nStart + 1);
}
// 角色名。
const char* RoleName(TenantRole role)
{
    return role == TenantRole::kOwner ? "owner" : "member";
}
}  // namespace

/// @brief 创建租户控制器。
CTenantsController::CTenantsController(ITenantService* pTenants) : m_pTenants(pTenants) {}

/// @brief 注册租户资源路由。
void CTenantsController::RegisterRoutes(web::CHttpRouter& router)
{
    router.Register({"POST", "/api/tenant",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleCreate(req, resp); }});
    router.Register({"GET", "/api/tenant/info",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleInfo(req, resp); }});
    router.Register({"POST", "/api/tenant/join",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleJoin(req, resp); }});
    router.Register({"GET", "/api/tenant/members",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleMembers(req, resp); }});
}

/// @brief 创建租户：POST /api/tenant —— 名称为请求体；创建者成为 Owner。
bool CTenantsController::HandleCreate(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pTenants == nullptr)
    {
        resp.WriteJson("{\"error\":\"tenant service unavailable\"}", "500");
        return true;
    }
    // 名称取请求体并去首尾空白。
    std::string strName = Trim(req.Body());
    if (strName.size() > 48)
    {
        strName = strName.substr(0, 48);
    }
    const std::string strOwner = AccountId(req);
    CTenant tenant;
    if (!m_pTenants->CreateTenant(strName, strOwner, tenant))
    {
        resp.WriteJson("{\"error\":\"create failed\"}", "500");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"code\":" << web::CHttpText::JsonString(tenant.strCode)
        << ",\"name\":" << web::CHttpText::JsonString(tenant.strName) << ",\"role\":\"" << RoleName(TenantRole::kOwner)
        << "\"}";
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

/// @brief 凭码加入：POST /api/tenant/join —— 目标码取 body（缺省 X-Tenant）。
bool CTenantsController::HandleJoin(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pTenants == nullptr)
    {
        resp.WriteJson("{\"error\":\"tenant service unavailable\"}", "500");
        return true;
    }
    std::string strCode = Trim(req.Body());
    if (strCode.empty())
    {
        strCode = req.Header("X-Tenant");
    }
    if (strCode.empty())
    {
        strCode = m_pTenants->DefaultCode();
    }
    TenantRole role = TenantRole::kMember;
    if (!m_pTenants->JoinTenant(strCode, AccountId(req), role))
    {
        resp.WriteJson("{\"error\":\"tenant not found\"}", "404");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"role\":\"" << RoleName(role) << "\"}";
    resp.WriteJson(oss.str());
    return true;
}

/// @brief 当前租户成员花名册：GET /api/tenant/members（当前 X-Tenant）。
bool CTenantsController::HandleMembers(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pTenants == nullptr)
    {
        resp.WriteJson("{\"error\":\"tenant service unavailable\"}", "500");
        return true;
    }
    const std::string strCode = RequestContextOf(req).Tenant().strCode;
    std::vector<CTenantMember> vecMembers;
    m_pTenants->ListMembers(strCode, vecMembers);
    std::ostringstream oss;
    oss << "{\"members\":[";
    bool bFirst = true;
    for (const CTenantMember& member : vecMembers)
    {
        if (!bFirst)
        {
            oss << ",";
        }
        bFirst = false;
        oss << "{\"account\":" << web::CHttpText::JsonString(member.strAccountId) << ",\"role\":\""
            << RoleName(member.role) << "\",\"joined\":" << member.nJoinMs << "}";
    }
    oss << "]}";
    resp.WriteJson(oss.str());
    return true;
}

}  // namespace datahub
