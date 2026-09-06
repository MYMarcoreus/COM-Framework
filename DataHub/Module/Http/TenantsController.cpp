#include "Module/Http/TenantsController.h"

#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "Framework/HttpRouter.h"
#include "Framework/HttpText.h"
#include "Log/Logger.h"

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
// 当前时间（毫秒）。
std::int64_t NowMs()
{
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
}
// 审计：设备侧租户操作落日志（op / target / actor）。
void AuditRecord(const std::string& strOp, const std::string& strTarget, const std::string& strActor)
{
    common::log::CLogger::Instance().Info("[Audit] op=" + strOp + " target=" + strTarget + " actor=" + strActor);
}
// 来源 IP（去掉端口，用于限流）。
std::string PeerIp(web::CHttpRequest& req)
{
    std::string strIp = req.Peer();
    const std::string::size_type nColon = strIp.find_last_of(':');
    if (nColon != std::string::npos)
    {
        strIp = strIp.substr(0, nColon);
    }
    return strIp;
}
// join 限流（防盲试短码；默认 ≤30 次/分钟/IP）。@return true = 超过阈值。
bool JoinRateLimited(const std::string& strKey)
{
    static std::mutex sMutex;
    static std::map<std::string, std::pair<std::int64_t, unsigned> > sWin;  // key → (windowStart, count)
    std::lock_guard<std::mutex> lock(sMutex);
    std::pair<std::int64_t, unsigned>& entry = sWin[strKey];
    if (NowMs() - entry.first > 60000)
    {
        entry.first = NowMs();
        entry.second = 0;
    }
    ++entry.second;
    return entry.second > 30;
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
    router.Register({"GET", "/api/tenant/state",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleState(req, resp); }});
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
    AuditRecord("tenant.create", tenant.strCode, strOwner);
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
    // B5：join 限流，防盲试 6 位短码枚举私人租户。
    if (JoinRateLimited(PeerIp(req)))
    {
        AuditRecord("tenant.join.ratelimited", strCode, AccountId(req));
        resp.WriteJson("{\"error\":\"too many join attempts\"}", "429");
        return true;
    }
    TenantRole role = TenantRole::kMember;
    if (!m_pTenants->JoinTenant(strCode, AccountId(req), role))
    {
        resp.WriteJson("{\"error\":\"tenant not found\"}", "404");
        return true;
    }
    AuditRecord("tenant.join", strCode, AccountId(req));
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

/// @brief 当前租户状态（对账）：返回 code/name/exists/role，供客户端检测改名/被踢/已删。
/// 租户不存在也返回 exists=false（由 OnRequest 对本路径放行），客户端据此回落并清理缓存。
bool CTenantsController::HandleState(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pTenants == nullptr)
    {
        resp.WriteJson("{\"error\":\"tenant service unavailable\"}", "500");
        return true;
    }
    const CRequestContext& ctx = RequestContextOf(req);
    const std::string strCode = ctx.Tenant().strCode;
    const bool bDefault = strCode == m_pTenants->DefaultCode();

    CTenant tenant;
    const bool bExists = !strCode.empty() && m_pTenants->FindTenant(strCode, tenant);
    std::string strRole;  // 空 = 非成员（被踢）或租户不存在
    if (bExists)
    {
        if (bDefault)
        {
            strRole = "member";  // 公共租户人人皆成员
        }
        else
        {
            TenantRole role = TenantRole::kMember;
            if (m_pTenants->TenantRoleOf(strCode, ctx.strAccountId, role))
            {
                strRole = RoleName(role);
            }
        }
    }
    std::ostringstream oss;
    oss << "{\"code\":" << web::CHttpText::JsonString(strCode)
        << ",\"name\":" << web::CHttpText::JsonString(bExists ? tenant.strName : std::string())
        << ",\"exists\":" << (bExists ? "true" : "false") << ",\"isDefault\":" << (bDefault ? "true" : "false")
        << ",\"role\":" << web::CHttpText::JsonString(strRole) << "}";
    resp.WriteJson(oss.str());
    return true;
}

}  // namespace datahub
