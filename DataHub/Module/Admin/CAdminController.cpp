#include "Module/Admin/CAdminController.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Framework/HttpRouter.h"
#include "Framework/HttpText.h"

namespace datahub {

using sc::CTenant;
using sc::CTenantLimits;
using sc::CTenantMember;
using sc::DataItemInfo;
using sc::IDataStore;
using sc::ITenantService;
using sc::TenantRole;

namespace {

// 角色名。
const char* RoleName(TenantRole role)
{
    return role == TenantRole::kOwner ? "owner" : "member";
}
// 解析 x-www-form-urlencoded 请求体 → 字段表（自动 URL 解码）。
std::map<std::string, std::string> ParseForm(const std::string& strBody)
{
    std::map<std::string, std::string> mapFields;
    std::string::size_type nPos = 0;
    while (nPos <= strBody.size())
    {
        std::string::size_type nAmp = strBody.find('&', nPos);
        std::string strPair = nAmp == std::string::npos ? strBody.substr(nPos) : strBody.substr(nPos, nAmp - nPos);
        std::string::size_type nEq = strPair.find('=');
        if (nEq != std::string::npos)
        {
            mapFields[web::CHttpText::UrlDecode(strPair.substr(0, nEq))] =
                web::CHttpText::UrlDecode(strPair.substr(nEq + 1));
        }
        if (nAmp == std::string::npos)
        {
            break;
        }
        nPos = nAmp + 1;
    }
    return mapFields;
}
// 数字解析：失败返回 0。
std::uint64_t ParseUint(const std::string& str)
{
    std::uint64_t nValue = 0;
    for (char c : str)
    {
        if (c < '0' || c > '9')
        {
            return 0;
        }
        nValue = nValue * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return nValue;
}

// —— JSON 拼装小件（复用 web::CHttpText::JsonString 做字符串转义） ——
void AppendMemberJson(std::ostringstream& oss, const CTenantMember& member)
{
    oss << "{\"account\":" << web::CHttpText::JsonString(member.strAccountId) << ",\"role\":\"" << RoleName(member.role)
        << "\",\"joined\":" << member.nJoinMs << "}";
}
void AppendItemJson(std::ostringstream& oss, const DataItemInfo& item)
{
    oss << "{\"id\":" << web::CHttpText::JsonString(item.strId) << ",\"kind\":\""
        << (item.kind == sc::DataKind::kText ? "text" : "file")
        << "\",\"name\":" << web::CHttpText::JsonString(item.strName)
        << ",\"from\":" << web::CHttpText::JsonString(item.strFrom) << ",\"size\":" << item.nSize
        << ",\"created\":" << item.nCreateMs << "}";
}
}  // namespace

/// @brief 创建管理控制器。
CAdminController::CAdminController(IDataStore* pStore, ITenantService* pTenants)
    : m_pStore(pStore), m_pTenants(pTenants)
{}

/// @brief 注册管理路由。
void CAdminController::RegisterRoutes(web::CHttpRouter& router)
{
    router.Register({"GET", "/api/admin/overview",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleOverview(req, resp); }});
    router.Register({"GET", "/api/admin/tenant",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleTenant(req, resp); }});
    router.Register({"DELETE", "/api/admin/tenant", [this](web::CHttpRequest& req, web::CHttpResponse& resp)
    { return HandleTenantDelete(req, resp); }});
    router.Register({"POST", "/api/admin/tenant/rename",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleRename(req, resp); }});
    router.Register({"POST", "/api/admin/tenant/limits",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleLimits(req, resp); }});
    router.Register({"GET", "/api/admin/item",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleItem(req, resp); }});
    router.Register({"DELETE", "/api/admin/item",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleItemDelete(req, resp); }});
    router.Register({"DELETE", "/api/admin/member", [this](web::CHttpRequest& req, web::CHttpResponse& resp)
    { return HandleMemberDelete(req, resp); }});
    router.Register({"POST", "/api/admin/member/role",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleMemberRole(req, resp); }});
}

/// @brief 全部租户一览 + 汇总统计。
bool CAdminController::HandleOverview(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    (void)req;
    std::vector<CTenant> vecTenants;
    if (m_pTenants != nullptr)
    {
        m_pTenants->ListTenants(vecTenants);
    }
    std::ostringstream oss;
    oss << "{\"tenants\":[";
    bool bFirst = true;
    std::uint64_t nTotalItems = 0;
    std::uint64_t nTotalBytes = 0;
    for (const CTenant& tenant : vecTenants)
    {
        if (m_pStore == nullptr)
        {
            continue;
        }
        const std::size_t nItems = m_pStore->Count(tenant);
        const std::uint64_t nBytes = m_pStore->TotalBytes(tenant);
        nTotalItems += nItems;
        nTotalBytes += nBytes;
        if (!bFirst)
        {
            oss << ",";
        }
        bFirst = false;
        oss << "{\"code\":" << web::CHttpText::JsonString(tenant.strCode)
            << ",\"name\":" << web::CHttpText::JsonString(tenant.strName) << ",\"isDefault\":"
            << (m_pTenants != nullptr && tenant.strCode == m_pTenants->DefaultCode() ? "true" : "false")
            << ",\"maxItems\":" << tenant.limits.nMaxItems << ",\"maxTotalBytes\":" << tenant.limits.nMaxTotalBytes
            << ",\"maxItemBytes\":" << tenant.limits.nMaxItemBytes << ",\"created\":" << tenant.nCreateMs
            << ",\"members\":" << [this, &tenant]()
        {
            std::vector<CTenantMember> vecMembers;
            std::size_t nMembers = 0;
            if (m_pTenants != nullptr)
            {
                m_pTenants->ListMembers(tenant.strCode, vecMembers);
                nMembers = vecMembers.size();
            }
            return nMembers;
        }() << ",\"items\":" << nItems << ",\"bytes\":" << nBytes << "}";
    }
    oss << "],\"count\":" << vecTenants.size() << ",\"totalItems\":" << nTotalItems << ",\"totalBytes\":" << nTotalBytes
        << "}";
    resp.WriteJson(oss.str());
    return true;
}

/// @brief 租户详情（信息 + 成员 + 数据条目）。
bool CAdminController::HandleTenant(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    const std::string strCode = req.QueryParam("code");
    if (strCode.empty())
    {
        resp.WriteJson("{\"error\":\"missing code\"}", "400");
        return true;
    }
    CTenant tenant;
    if (m_pTenants == nullptr || !m_pTenants->FindTenant(strCode, tenant))
    {
        resp.WriteJson("{\"error\":\"tenant not found\"}", "404");
        return true;
    }
    std::vector<CTenantMember> vecMembers;
    if (m_pTenants != nullptr)
    {
        m_pTenants->ListMembers(strCode, vecMembers);
    }
    std::vector<DataItemInfo> vecItems;
    if (m_pStore != nullptr)
    {
        vecItems = m_pStore->List(tenant);
    }
    std::ostringstream oss;
    oss << "{\"code\":" << web::CHttpText::JsonString(tenant.strCode)
        << ",\"name\":" << web::CHttpText::JsonString(tenant.strName) << ",\"isDefault\":"
        << (m_pTenants != nullptr && tenant.strCode == m_pTenants->DefaultCode() ? "true" : "false")
        << ",\"maxItems\":" << tenant.limits.nMaxItems << ",\"maxTotalBytes\":" << tenant.limits.nMaxTotalBytes
        << ",\"maxItemBytes\":" << tenant.limits.nMaxItemBytes << ",\"created\":" << tenant.nCreateMs
        << ",\"members\":[";
    bool bFirst = true;
    for (const CTenantMember& member : vecMembers)
    {
        if (!bFirst)
        {
            oss << ",";
        }
        bFirst = false;
        AppendMemberJson(oss, member);
    }
    oss << "],\"items\":[";
    bFirst = true;
    for (const DataItemInfo& item : vecItems)
    {
        if (!bFirst)
        {
            oss << ",";
        }
        bFirst = false;
        AppendItemJson(oss, item);
    }
    oss << "],\"count\":" << vecItems.size()
        << ",\"bytes\":" << (m_pStore != nullptr ? m_pStore->TotalBytes(tenant) : 0) << "}";
    resp.WriteJson(oss.str());
    return true;
}

/// @brief 删除租户（并清空其数据；公共租户不可删）。
bool CAdminController::HandleTenantDelete(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    const std::string strCode = req.QueryParam("code");
    if (strCode.empty())
    {
        resp.WriteJson("{\"error\":\"missing code\"}", "400");
        return true;
    }
    CTenant tenant;
    if (m_pTenants == nullptr || !m_pTenants->FindTenant(strCode, tenant))
    {
        resp.WriteJson("{\"error\":\"tenant not found\"}", "404");
        return true;
    }
    if (strCode == m_pTenants->DefaultCode())
    {
        resp.WriteJson("{\"error\":\"default tenant cannot be removed\"}", "400");
        return true;
    }
    if (!m_pTenants->RemoveTenant(strCode))
    {
        resp.WriteJson("{\"error\":\"remove failed\"}", "500");
        return true;
    }
    if (m_pStore != nullptr)
    {
        m_pStore->PurgeTenant(tenant);  // 清空该租户全部数据
    }
    resp.WriteJson("{\"ok\":true}");
    return true;
}

/// @brief 重命名租户。
bool CAdminController::HandleRename(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    std::map<std::string, std::string> mapFields = ParseForm(req.Body());
    const std::string strCode = mapFields["code"];
    const std::string strName = mapFields["name"];
    if (strCode.empty() || strName.empty())
    {
        resp.WriteJson("{\"error\":\"missing code or name\"}", "400");
        return true;
    }
    if (m_pTenants == nullptr || !m_pTenants->RenameTenant(strCode, strName))
    {
        resp.WriteJson("{\"error\":\"rename failed\"}", "400");
        return true;
    }
    resp.WriteJson("{\"ok\":true}");
    return true;
}

/// @brief 调整配额（0 = 不限制）。
bool CAdminController::HandleLimits(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    std::map<std::string, std::string> mapFields = ParseForm(req.Body());
    const std::string strCode = mapFields["code"];
    if (strCode.empty())
    {
        resp.WriteJson("{\"error\":\"missing code\"}", "400");
        return true;
    }
    CTenantLimits limits;
    limits.nMaxItems = static_cast<std::size_t>(ParseUint(mapFields["maxItems"]));
    limits.nMaxTotalBytes = ParseUint(mapFields["maxTotalBytes"]);
    limits.nMaxItemBytes = ParseUint(mapFields["maxItemBytes"]);
    if (m_pTenants == nullptr || !m_pTenants->SetTenantLimits(strCode, limits))
    {
        resp.WriteJson("{\"error\":\"tenant not found\"}", "404");
        return true;
    }
    resp.WriteJson("{\"ok\":true}");
    return true;
}

/// @brief 数据条目元信息（文本附内容预览，截断 2000 字符）。
bool CAdminController::HandleItem(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    const std::string strCode = req.QueryParam("code");
    const std::string strId = req.QueryParam("id");
    if (strCode.empty() || strId.empty())
    {
        resp.WriteJson("{\"error\":\"missing code or id\"}", "400");
        return true;
    }
    CTenant tenant;
    if (m_pTenants == nullptr || !m_pTenants->FindTenant(strCode, tenant))
    {
        resp.WriteJson("{\"error\":\"tenant not found\"}", "404");
        return true;
    }
    if (m_pStore == nullptr)
    {
        resp.WriteJson("{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    DataItemInfo info;
    if (!m_pStore->GetInfo(tenant, strId, info))
    {
        resp.WriteJson("{\"error\":\"item not found\"}", "404");
        return true;
    }
    std::ostringstream oss;
    AppendItemJson(oss, info);
    std::string strJson = oss.str();
    strJson.erase(strJson.size() - 1);  // 去掉结尾 '}'
    if (info.kind == sc::DataKind::kText)
    {
        std::string strText;
        m_pStore->GetText(tenant, strId, strText);
        if (strText.size() > 2000)
        {
            strText.resize(2000);
        }
        strJson += ",\"text\":" + web::CHttpText::JsonString(strText);
    }
    strJson += "}";
    resp.WriteJson(strJson);
    return true;
}

/// @brief 删除数据条目。
bool CAdminController::HandleItemDelete(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    const std::string strCode = req.QueryParam("code");
    const std::string strId = req.QueryParam("id");
    if (strCode.empty() || strId.empty())
    {
        resp.WriteJson("{\"error\":\"missing code or id\"}", "400");
        return true;
    }
    CTenant tenant;
    if (m_pTenants == nullptr || !m_pTenants->FindTenant(strCode, tenant))
    {
        resp.WriteJson("{\"error\":\"tenant not found\"}", "404");
        return true;
    }
    if (m_pStore == nullptr || !m_pStore->Remove(tenant, strId))
    {
        resp.WriteJson("{\"error\":\"item not found\"}", "404");
        return true;
    }
    resp.WriteJson("{\"ok\":true}");
    return true;
}

/// @brief 移除成员。
bool CAdminController::HandleMemberDelete(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    const std::string strCode = req.QueryParam("code");
    const std::string strAccount = req.QueryParam("account");
    if (strCode.empty() || strAccount.empty())
    {
        resp.WriteJson("{\"error\":\"missing code or account\"}", "400");
        return true;
    }
    if (m_pTenants == nullptr || !m_pTenants->RemoveMember(strCode, strAccount))
    {
        resp.WriteJson("{\"error\":\"remove member failed (keep >= 1 owner)\"}", "400");
        return true;
    }
    resp.WriteJson("{\"ok\":true}");
    return true;
}

/// @brief 设置成员角色（owner/member）。
bool CAdminController::HandleMemberRole(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    std::map<std::string, std::string> mapFields = ParseForm(req.Body());
    const std::string strCode = mapFields["code"];
    const std::string strAccount = mapFields["account"];
    const std::string strRole = mapFields["role"];
    if (strCode.empty() || strAccount.empty() || (strRole != "owner" && strRole != "member"))
    {
        resp.WriteJson("{\"error\":\"missing code/account/role\"}", "400");
        return true;
    }
    const TenantRole role = strRole == "owner" ? TenantRole::kOwner : TenantRole::kMember;
    if (m_pTenants == nullptr || !m_pTenants->SetMemberRole(strCode, strAccount, role))
    {
        resp.WriteJson("{\"error\":\"set role failed (target must be member; keep >= 1 owner)\"}", "400");
        return true;
    }
    resp.WriteJson("{\"ok\":true}");
    return true;
}

}  // namespace datahub
