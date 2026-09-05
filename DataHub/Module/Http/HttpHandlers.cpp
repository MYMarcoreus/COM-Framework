#include "Module/Http/HttpHandlers.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Framework/HttpRouter.h"
#include "Framework/HttpText.h"
#include "Module/Http/MemberService.h"
#include "Module/Tenant/ITenantService.h"

namespace datahub {

using sc::CTenant;
using sc::DataItemInfo;
using sc::DataKind;
using sc::ITenantService;

namespace {
// 取当前请求所属租户：入口 OnRequest 解析后挂在请求上下文（UserData）。
const CTenant& RequestTenant(web::CHttpRequest& req)
{
    const CTenant* pTenant = static_cast<const CTenant*>(req.UserData());
    if (pTenant != nullptr)
    {
        return *pTenant;
    }
    // 兜底（正常流程不会触发）。
    static const CTenant kFallback;
    return kFallback;
}
}  // namespace

CHttpHandlers::CHttpHandlers(sc::IDataStore* pStore, CMemberService* pMembers, ITenantService* pTenants,
                             std::uint64_t nMaxBodyBytes)
    : m_pStore(pStore), m_pMembers(pMembers), m_pTenants(pTenants), m_nMaxBodyBytes(nMaxBodyBytes)
{}

/// @brief 注册本控制器负责的全部业务路由（装配层 Initialize 时调用）。
void CHttpHandlers::RegisterRoutes(web::CHttpRouter& router)
{
    // 路径模板：字面段精确匹配；"{id}" 段捕获，分发后经 req.PathParam() 读取。
    router.Register({"GET", "/api/list",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleList(req, resp); }});
    router.Register({"GET", "/api/members",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleMembers(req, resp); }});
    router.Register({"POST", "/api/text",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleUploadText(req, resp); }});
    router.Register({"GET", "/api/text/{id}",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleGetText(req, resp); }});
    router.Register({"POST", "/api/file",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleUploadFile(req, resp); }});
    router.Register({"GET", "/api/file/{id}",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleGetFile(req, resp); }});
    router.Register({"DELETE", "/api/item/{id}",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleDelete(req, resp); }});
    // —— 空间（租户）域 ——
    router.Register({"POST", "/api/space",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleCreateSpace(req, resp); }});
    router.Register({"GET", "/api/space/info",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleSpaceInfo(req, resp); }});
}

// ----------------------------------------------------------------------------
// 列表：GET /api/list —— 返回消息 JSON 数组
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleList(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pStore == nullptr)
    {
        resp.WriteJson("{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    // 游标增量同步：?since=<seq> 只拉新增（按序号升序，旧→新）；缺省返回全量。
    std::uint64_t nSince = 0;
    std::string strSince = req.QueryParam("since");
    if (!strSince.empty())
    {
        std::uint64_t nParsed = 0;
        bool bValid = true;
        for (char c : strSince)
        {
            if (c < '0' || c > '9')
            {
                bValid = false;
                break;
            }
            nParsed = nParsed * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (bValid)
        {
            nSince = nParsed;
        }
    }
    const CTenant& tenant = RequestTenant(req);
    std::vector<DataItemInfo> vecItems = m_pStore->ListSince(tenant, nSince);
    std::ostringstream oss;
    oss << "{\"items\":[";
    bool bFirst = true;
    for (const DataItemInfo& info : vecItems)
    {
        if (!bFirst)
        {
            oss << ",";
        }
        bFirst = false;
        oss << "{\"id\":" << web::CHttpText::JsonString(info.strId)
            << ",\"type\":" << web::CHttpText::JsonString(info.kind == DataKind::kText ? "text" : "file")
            << ",\"name\":" << web::CHttpText::JsonString(info.strName)
            << ",\"from\":" << web::CHttpText::JsonString(info.strFrom) << ",\"size\":" << info.nSize
            << ",\"seq\":" << info.nSeq << ",\"time\":" << info.nCreateMs << "}";
    }
    oss << "]}";
    resp.WriteJson(oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 在线成员：GET /api/members —— 返回成员列表（按最后活跃倒序）
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleMembers(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pMembers == nullptr)
    {
        resp.WriteJson("{\"members\":[]}");
        return true;
    }
    m_pMembers->Prune();
    const CTenant& tenant = RequestTenant(req);
    std::map<std::string, CMemberService::MemberInfo> mapMembers = m_pMembers->Snapshot(tenant);
    std::ostringstream oss;
    oss << "{\"members\":[";
    bool bFirst = true;
    std::vector<std::pair<std::string, CMemberService::MemberInfo> > vecSorted(mapMembers.begin(), mapMembers.end());
    std::sort(vecSorted.begin(), vecSorted.end(),
              [](const std::pair<std::string, CMemberService::MemberInfo>& a,
                 const std::pair<std::string, CMemberService::MemberInfo>& b)
    { return a.second.nLastMs > b.second.nLastMs; });
    for (const auto& pair : vecSorted)
    {
        if (!bFirst)
        {
            oss << ",";
        }
        bFirst = false;
        oss << "{\"id\":" << web::CHttpText::JsonString(pair.first)
            << ",\"ip\":" << web::CHttpText::JsonString(pair.second.strIp) << ",\"first\":" << pair.second.nFirstMs
            << ",\"last\":" << pair.second.nLastMs << "}";
    }
    oss << "]}";
    resp.WriteJson(oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 上传文本：POST /api/text —— body 为文本内容
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleUploadText(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pStore == nullptr)
    {
        resp.WriteJson("{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strBody = req.Body();
    if (strBody.empty())
    {
        resp.WriteJson("{\"error\":\"empty body\"}", "400");
        return true;
    }
    if (m_nMaxBodyBytes > 0 && static_cast<std::uint64_t>(strBody.size()) > m_nMaxBodyBytes)
    {
        resp.WriteJson("{\"error\":\"body too large\"}", "413");
        return true;
    }
    std::string strFrom = CMemberService::ClientId(req);
    const CTenant& tenant = RequestTenant(req);
    std::string strId = m_pStore->SaveText(tenant, strBody, strFrom);
    if (strId.empty())
    {
        resp.WriteJson("{\"error\":\"save failed\"}", "500");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"id\":" << web::CHttpText::JsonString(strId) << "}";
    resp.WriteJson(oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 获取文本：GET /api/text/<id>
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleGetText(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pStore == nullptr)
    {
        resp.WriteJson("{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    const CTenant& tenant = RequestTenant(req);
    std::string strId = web::CHttpText::UrlDecode(req.PathParam());
    std::string strText;
    if (!m_pStore->GetText(tenant, strId, strText))
    {
        resp.WriteJson("{\"error\":\"not found\"}", "404");
        return true;
    }
    resp.WriteText(strText);
    return true;
}

// ----------------------------------------------------------------------------
// 上传文件：POST /api/file —— header X-File-Name 指定文件名，body 为内容
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleUploadFile(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pStore == nullptr)
    {
        resp.WriteJson("{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strFileName = web::CHttpText::UrlDecode(req.Header("X-File-Name"));
    std::string strBody = req.Body();
    if (strBody.empty())
    {
        resp.WriteJson("{\"error\":\"empty body\"}", "400");
        return true;
    }
    if (m_nMaxBodyBytes > 0 && static_cast<std::uint64_t>(strBody.size()) > m_nMaxBodyBytes)
    {
        resp.WriteJson("{\"error\":\"body too large\"}", "413");
        return true;
    }
    std::string strFrom = CMemberService::ClientId(req);
    const CTenant& tenant = RequestTenant(req);
    std::string strId = m_pStore->SaveFile(tenant, strFileName, strBody.data(), strBody.size(), strFrom);
    if (strId.empty())
    {
        resp.WriteJson("{\"error\":\"save failed\"}", "500");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"id\":" << web::CHttpText::JsonString(strId) << "}";
    resp.WriteJson(oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 下载文件 / 图片：GET /api/file/<id> —— 内联图片或附件下载，支持 Range 分段
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleGetFile(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pStore == nullptr)
    {
        resp.WriteJson("{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    const CTenant& tenant = RequestTenant(req);
    std::string strId = web::CHttpText::UrlDecode(req.PathParam());
    std::string strName;
    std::vector<char> vecData;
    if (!m_pStore->GetFile(tenant, strId, strName, vecData))
    {
        resp.WriteJson("{\"error\":\"not found\"}", "404");
        return true;
    }
    std::string strRange = req.Header("Range");
    resp.WriteFile(strName, vecData.data(), vecData.size(), strRange);
    return true;
}

// ----------------------------------------------------------------------------
// 删除：DELETE /api/item/<id>
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleDelete(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pStore == nullptr)
    {
        resp.WriteJson("{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    const CTenant& tenant = RequestTenant(req);
    std::string strId = web::CHttpText::UrlDecode(req.PathParam());
    // 归属校验：仅允许删除自己创建的数据项；来源为空的历史数据项可任意删。
    DataItemInfo info;
    if (!m_pStore->GetInfo(tenant, strId, info))
    {
        resp.WriteJson("{\"error\":\"not found\"}", "404");
        return true;
    }
    std::string strRequester = CMemberService::ClientId(req);
    if (!info.strFrom.empty() && info.strFrom != strRequester)
    {
        resp.WriteJson("{\"error\":\"forbidden\"}", "403");
        return true;
    }
    if (!m_pStore->Remove(tenant, strId))
    {
        resp.WriteJson("{\"error\":\"not found\"}", "404");
        return true;
    }
    resp.WriteJson("{\"ok\":true}");
    return true;
}

// ----------------------------------------------------------------------------
// 创建空间（租户）：POST /api/space —— 名称为请求体（UTF-8，非空；空则用默认名）
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleCreateSpace(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pTenants == nullptr)
    {
        resp.WriteJson("{\"error\":\"tenant service unavailable\"}", "500");
        return true;
    }
    // 名称取请求体（workflow 拒绝零长度 POST body，故 body 恒非空）。
    std::string strName = req.Body();
    // 去掉首尾空白（含换行）。
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

// ----------------------------------------------------------------------------
// 查询空间信息：GET /api/space/info?code=xxx —— 供"凭码加入"前校验存在性
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleSpaceInfo(web::CHttpRequest& req, web::CHttpResponse& resp)
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
        resp.WriteJson("{\"error\":\"space not found\"}", "404");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"code\":" << web::CHttpText::JsonString(tenant.strCode)
        << ",\"name\":" << web::CHttpText::JsonString(tenant.strName) << ",\"created\":" << tenant.nCreateMs
        << "}";
    resp.WriteJson(oss.str());
    return true;
}

}  // namespace datahub
