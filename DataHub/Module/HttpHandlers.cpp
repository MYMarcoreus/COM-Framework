#include "Module/HttpHandlers.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Framework/HttpText.h"
#include "Module/MemberService.h"

namespace datahub {

using sc::DataItemInfo;
using sc::DataKind;

CHttpHandlers::CHttpHandlers(sc::IDataStore* pStore, CMemberService* pMembers)
    : m_pStore(pStore), m_pMembers(pMembers)
{
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
    std::vector<DataItemInfo> vecItems = m_pStore->List();
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
        oss << "{\"id\":\"" << info.strId << "\"" << ",\"type\":\"" << (info.kind == DataKind::kText ? "text" : "file")
            << "\"" << ",\"name\":\"" << web::CHttpText::HtmlEscape(info.strName) << "\"" << ",\"from\":\""
            << web::CHttpText::HtmlEscape(info.strFrom) << "\"" << ",\"size\":" << info.nSize
            << ",\"time\":" << info.nCreateMs << "}";
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
    std::map<std::string, CMemberService::MemberInfo> mapMembers = m_pMembers->Snapshot();
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
        oss << "{\"id\":\"" << web::CHttpText::HtmlEscape(pair.first) << "\"" << ",\"ip\":\""
            << web::CHttpText::HtmlEscape(pair.second.strIp) << "\"" << ",\"first\":" << pair.second.nFirstMs
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
    std::string strFrom = CMemberService::ClientId(req);
    std::string strId = m_pStore->SaveText(strBody, strFrom);
    if (strId.empty())
    {
        resp.WriteJson("{\"error\":\"save failed\"}", "500");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"id\":\"" << strId << "\"}";
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
    std::string strId = web::CHttpText::UrlDecode(req.PathParam());
    std::string strText;
    if (!m_pStore->GetText(strId, strText))
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
    std::string strFrom = CMemberService::ClientId(req);
    std::string strId = m_pStore->SaveFile(strFileName, strBody.data(), strBody.size(), strFrom);
    if (strId.empty())
    {
        resp.WriteJson("{\"error\":\"save failed\"}", "500");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"id\":\"" << strId << "\"}";
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
    std::string strId = web::CHttpText::UrlDecode(req.PathParam());
    std::string strName;
    std::vector<char> vecData;
    if (!m_pStore->GetFile(strId, strName, vecData))
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
    std::string strId = web::CHttpText::UrlDecode(req.PathParam());
    if (!m_pStore->Remove(strId))
    {
        resp.WriteJson("{\"error\":\"not found\"}", "404");
        return true;
    }
    resp.WriteJson("{\"ok\":true}");
    return true;
}

}  // namespace datahub
