#include "Module/HttpHandlers.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include "Framework/HttpUtil.h"
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
bool CHttpHandlers::HandleList(WFHttpTask* pServerTask)
{
    if (m_pStore == nullptr)
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
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
            << "\"" << ",\"name\":\"" << web::CHttpUtil::HtmlEscape(info.strName) << "\"" << ",\"from\":\""
            << web::CHttpUtil::HtmlEscape(info.strFrom) << "\"" << ",\"size\":" << info.nSize
            << ",\"time\":" << info.nCreateMs << "}";
    }
    oss << "]}";
    web::CHttpUtil::WriteJson(pServerTask, oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 在线成员：GET /api/members —— 返回成员列表（按最后活跃倒序）
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleMembers(WFHttpTask* pServerTask)
{
    if (m_pMembers == nullptr)
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"members\":[]}");
        return true;
    }
    m_pMembers->Prune();
    std::map<std::string, CMemberService::MemberInfo> mapMembers = m_pMembers->Snapshot();
    std::ostringstream oss;
    oss << "{\"members\":[";
    bool bFirst = true;
    // 按最后活跃时间倒序（最近活跃在前）。
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
        oss << "{\"id\":\"" << web::CHttpUtil::HtmlEscape(pair.first) << "\"" << ",\"ip\":\""
            << web::CHttpUtil::HtmlEscape(pair.second.strIp) << "\"" << ",\"first\":" << pair.second.nFirstMs
            << ",\"last\":" << pair.second.nLastMs << "}";
    }
    oss << "]}";
    web::CHttpUtil::WriteJson(pServerTask, oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 上传文本：POST /api/text —— body 为文本内容
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleUploadText(WFHttpTask* pServerTask)
{
    if (m_pStore == nullptr)
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strBody;
    web::CHttpUtil::ReadBody(pServerTask, strBody);
    if (strBody.empty())
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"empty body\"}", "400");
        return true;
    }
    std::string strFrom = CMemberService::ClientId(pServerTask);
    std::string strId = m_pStore->SaveText(strBody, strFrom);
    if (strId.empty())
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"save failed\"}", "500");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"id\":\"" << strId << "\"}";
    web::CHttpUtil::WriteJson(pServerTask, oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 获取文本：GET /api/text/<id>
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleGetText(WFHttpTask* pServerTask, const std::string& strId)
{
    if (m_pStore == nullptr)
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strText;
    if (!m_pStore->GetText(strId, strText))
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"not found\"}", "404");
        return true;
    }
    web::CHttpUtil::WriteText(pServerTask, strText);
    return true;
}

// ----------------------------------------------------------------------------
// 上传文件：POST /api/file —— header X-File-Name 指定文件名，body 为内容
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleUploadFile(WFHttpTask* pServerTask)
{
    if (m_pStore == nullptr)
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strFileName = web::CHttpUtil::GetHeader(pServerTask, "X-File-Name");
    strFileName = web::CHttpUtil::UrlDecode(strFileName);
    std::string strBody;
    size_t nSize = web::CHttpUtil::ReadBody(pServerTask, strBody);
    if (nSize == 0)
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"empty body\"}", "400");
        return true;
    }
    std::string strFrom = CMemberService::ClientId(pServerTask);
    std::string strId = m_pStore->SaveFile(strFileName, strBody.data(), strBody.size(), strFrom);
    if (strId.empty())
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"save failed\"}", "500");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"id\":\"" << strId << "\"}";
    web::CHttpUtil::WriteJson(pServerTask, oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 下载文件 / 图片：GET /api/file/<id> —— 内联图片或附件下载，支持 Range 分段
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleGetFile(WFHttpTask* pServerTask, const std::string& strId)
{
    if (m_pStore == nullptr)
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strName;
    std::vector<char> vecData;
    if (!m_pStore->GetFile(strId, strName, vecData))
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"not found\"}", "404");
        return true;
    }
    std::string strRange = web::CHttpUtil::GetHeader(pServerTask, "Range");
    web::CHttpUtil::WriteFile(pServerTask, strName, vecData.data(), vecData.size(), strRange);
    return true;
}

// ----------------------------------------------------------------------------
// 删除：DELETE /api/item/<id>
// ----------------------------------------------------------------------------
bool CHttpHandlers::HandleDelete(WFHttpTask* pServerTask, const std::string& strId)
{
    if (m_pStore == nullptr)
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    if (!m_pStore->Remove(strId))
    {
        web::CHttpUtil::WriteJson(pServerTask, "{\"error\":\"not found\"}", "404");
        return true;
    }
    web::CHttpUtil::WriteJson(pServerTask, "{\"ok\":true}");
    return true;
}

}  // namespace datahub
