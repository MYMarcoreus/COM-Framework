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
#include "Module/Http/RequestContext.h"

namespace datahub {

using sc::CTenant;
using sc::DataItemInfo;
using sc::DataKind;

CHttpHandlers::CHttpHandlers(sc::IDataStore* pStore, CMemberService* pMembers, std::uint64_t nMaxBodyBytes)
    : m_pStore(pStore), m_pMembers(pMembers), m_nMaxBodyBytes(nMaxBodyBytes)
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
    const CTenant& tenant = RequestContextOf(req).Tenant();
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
    const CTenant& tenant = RequestContextOf(req).Tenant();
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
    const CTenant& tenant = RequestContextOf(req).Tenant();
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
    const CTenant& tenant = RequestContextOf(req).Tenant();
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
    const CTenant& tenant = RequestContextOf(req).Tenant();
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
    const CTenant& tenant = RequestContextOf(req).Tenant();
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
    const CTenant& tenant = RequestContextOf(req).Tenant();
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

}  // namespace datahub
