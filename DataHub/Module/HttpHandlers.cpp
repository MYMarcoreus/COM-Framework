#include "Module/HttpHandlers.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "Module/HttpUtil.h"
#include "Module/MemberTracker.h"
#include "workflow/HttpMessage.h"

namespace datahub {

using sc::DataItemInfo;
using sc::DataKind;

sc::IDataStore* HttpHandlers::s_pStore = nullptr;
const std::string* HttpHandlers::s_pIndexHtml = nullptr;

void HttpHandlers::SetStore(sc::IDataStore* pStore)
{
    s_pStore = pStore;
}

void HttpHandlers::SetIndexHtml(const std::string* pIndexHtml)
{
    s_pIndexHtml = pIndexHtml;
}

// ----------------------------------------------------------------------------
// 首页：返回前端页面（独立资源文件，Initialize 时加载）
// ----------------------------------------------------------------------------
bool HttpHandlers::HandleIndex(WFHttpTask* pServerTask)
{
    protocol::HttpResponse* pResp = pServerTask->get_resp();
    if (s_pIndexHtml == nullptr || s_pIndexHtml->empty())
    {
        pResp->set_status_code("503");
        pResp->add_header_pair("Content-Type", "text/plain; charset=utf-8");
        pResp->append_output_body("前端页面未加载");
        return true;
    }
    pResp->set_status_code("200");
    pResp->add_header_pair("Content-Type", "text/html; charset=utf-8");
    pResp->append_output_body(s_pIndexHtml->data(), s_pIndexHtml->size());
    return true;
}

// ----------------------------------------------------------------------------
// 列表：GET /api/list —— 返回消息 JSON 数组
// ----------------------------------------------------------------------------
bool HttpHandlers::HandleList(WFHttpTask* pServerTask)
{
    if (s_pStore == nullptr)
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::vector<DataItemInfo> vecItems = s_pStore->List();
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
            << "\"" << ",\"name\":\"" << HttpUtil::HtmlEscape(info.strName) << "\"" << ",\"from\":\""
            << HttpUtil::HtmlEscape(info.strFrom) << "\"" << ",\"size\":" << info.nSize
            << ",\"time\":" << info.nCreateMs << "}";
    }
    oss << "]}";
    HttpUtil::WriteJson(pServerTask, oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 在线成员：GET /api/members —— 返回成员列表（按最后活跃倒序）
// ----------------------------------------------------------------------------
bool HttpHandlers::HandleMembers(WFHttpTask* pServerTask)
{
    MemberTracker::Prune();
    std::map<std::string, MemberTracker::MemberInfo> mapMembers = MemberTracker::Snapshot();
    std::ostringstream oss;
    oss << "{\"members\":[";
    bool bFirst = true;
    // 按最后活跃时间倒序（最近活跃在前）。
    std::vector<std::pair<std::string, MemberTracker::MemberInfo> > vecSorted(mapMembers.begin(), mapMembers.end());
    std::sort(vecSorted.begin(), vecSorted.end(),
              [](const std::pair<std::string, MemberTracker::MemberInfo>& a,
                 const std::pair<std::string, MemberTracker::MemberInfo>& b)
    { return a.second.nLastMs > b.second.nLastMs; });
    for (const auto& pair : vecSorted)
    {
        if (!bFirst)
        {
            oss << ",";
        }
        bFirst = false;
        oss << "{\"id\":\"" << HttpUtil::HtmlEscape(pair.first) << "\"" << ",\"ip\":\""
            << HttpUtil::HtmlEscape(pair.second.strIp) << "\"" << ",\"first\":" << pair.second.nFirstMs
            << ",\"last\":" << pair.second.nLastMs << "}";
    }
    oss << "]}";
    HttpUtil::WriteJson(pServerTask, oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 上传文本：POST /api/text —— body 为文本内容
// ----------------------------------------------------------------------------
bool HttpHandlers::HandleUploadText(WFHttpTask* pServerTask)
{
    if (s_pStore == nullptr)
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strBody;
    HttpUtil::ReadBody(pServerTask, strBody);
    if (strBody.empty())
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"empty body\"}", "400");
        return true;
    }
    std::string strFrom = MemberTracker::ClientId(pServerTask);
    std::string strId = s_pStore->SaveText(strBody, strFrom);
    if (strId.empty())
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"save failed\"}", "500");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"id\":\"" << strId << "\"}";
    HttpUtil::WriteJson(pServerTask, oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 获取文本：GET /api/text/<id>
// ----------------------------------------------------------------------------
bool HttpHandlers::HandleGetText(WFHttpTask* pServerTask, const std::string& strId)
{
    if (s_pStore == nullptr)
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strText;
    if (!s_pStore->GetText(strId, strText))
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"not found\"}", "404");
        return true;
    }
    HttpUtil::WriteText(pServerTask, strText);
    return true;
}

// ----------------------------------------------------------------------------
// 上传文件：POST /api/file —— header X-File-Name 指定文件名，body 为内容
// ----------------------------------------------------------------------------
bool HttpHandlers::HandleUploadFile(WFHttpTask* pServerTask)
{
    if (s_pStore == nullptr)
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strFileName = HttpUtil::GetHeader(pServerTask, "X-File-Name");
    // 前端上传时对文件名做 encodeURIComponent 编码（%XX），此处解码还原；
    // 对纯 ASCII 文件名解码是幂等的，直接解码安全。
    strFileName = HttpUtil::UrlDecode(strFileName);
    std::string strBody;
    size_t nSize = HttpUtil::ReadBody(pServerTask, strBody);
    if (nSize == 0)
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"empty body\"}", "400");
        return true;
    }
    std::string strFrom = MemberTracker::ClientId(pServerTask);
    std::string strId = s_pStore->SaveFile(strFileName, strBody.data(), strBody.size(), strFrom);
    if (strId.empty())
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"save failed\"}", "500");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"id\":\"" << strId << "\"}";
    HttpUtil::WriteJson(pServerTask, oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 下载文件 / 图片：GET /api/file/<id>
// ----------------------------------------------------------------------------
bool HttpHandlers::HandleGetFile(WFHttpTask* pServerTask, const std::string& strId)
{
    if (s_pStore == nullptr)
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strName;
    std::vector<char> vecData;
    if (!s_pStore->GetFile(strId, strName, vecData))
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"not found\"}", "404");
        return true;
    }
    protocol::HttpResponse* pResp = pServerTask->get_resp();
    pResp->set_status_code("200");

    // 根据文件类型区分响应：图片内联显示（供 <img> 渲染），其他文件下载。
    std::string strLower = strName;
    std::transform(strLower.begin(), strLower.end(), strLower.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });

    bool bIsImage = strLower.rfind(".png") == strLower.size() - 4 || strLower.rfind(".jpg") == strLower.size() - 4 ||
                    strLower.rfind(".jpeg") == strLower.size() - 5 || strLower.rfind(".gif") == strLower.size() - 4 ||
                    strLower.rfind(".webp") == strLower.size() - 5 || strLower.rfind(".bmp") == strLower.size() - 4 ||
                    strLower.rfind(".svg") == strLower.size() - 4;

    if (bIsImage)
    {
        // 图片内联：浏览器可直接 <img> 渲染。
        std::string strMime = "image/png";
        if (strLower.rfind(".jpg") == strLower.size() - 4 || strLower.rfind(".jpeg") == strLower.size() - 5)
        {
            strMime = "image/jpeg";
        }
        else if (strLower.rfind(".gif") == strLower.size() - 4)
        {
            strMime = "image/gif";
        }
        else if (strLower.rfind(".webp") == strLower.size() - 5)
        {
            strMime = "image/webp";
        }
        else if (strLower.rfind(".bmp") == strLower.size() - 4)
        {
            strMime = "image/bmp";
        }
        else if (strLower.rfind(".svg") == strLower.size() - 4)
        {
            strMime = "image/svg+xml";
        }
        pResp->add_header_pair("Content-Type", strMime.c_str());
        pResp->add_header_pair("Content-Disposition", "inline");
    }
    else
    {
        // 其他文件：下载。HTTP 头不允许非 ASCII 字节，中文名用 RFC 5987 编码。
        pResp->add_header_pair("Content-Type", "application/octet-stream");
        std::string strDisposition;
        if (HttpUtil::HasNonAscii(strName))
        {
            strDisposition = "attachment; filename=\"download.bin\"; filename*=UTF-8''" + HttpUtil::UrlEncode(strName);
        }
        else
        {
            strDisposition = "attachment; filename=\"" + strName + "\"";
        }
        pResp->add_header_pair("Content-Disposition", strDisposition.c_str());
    }

    // HTTP Range 分段支持：workflow 对单次超大响应体的发送在部分网络环境下会截断
    // （约 110KB 上限），故大文件采用分段传输：前端以 Range 分多段拉取再拼接。
    // 每次响应的 body 都小于阈值，绕开该问题且不修改 workflow 本身。
    std::string strRange = HttpUtil::GetHeader(pServerTask, "Range");
    size_t nTotal = vecData.size();
    size_t nStart = 0;
    size_t nEnd = nTotal > 0 ? nTotal - 1 : 0;

    // 解析 "bytes=start-end" 或 "bytes=start-"
    if (!strRange.empty() && strRange.compare(0, 6, "bytes=") == 0 && nTotal > 0)
    {
        std::string strSpec = strRange.substr(6);
        std::string::size_type nDash = strSpec.find('-');
        if (nDash != std::string::npos)
        {
            std::string strStart = strSpec.substr(0, nDash);
            std::string strEnd = strSpec.substr(nDash + 1);
            if (!strStart.empty())
            {
                size_t nReqStart = static_cast<size_t>(std::atoll(strStart.c_str()));
                if (nReqStart < nTotal)
                {
                    nStart = nReqStart;
                    if (!strEnd.empty())
                    {
                        size_t nReqEnd = static_cast<size_t>(std::atoll(strEnd.c_str()));
                        nEnd = nReqEnd < nTotal ? nReqEnd : nTotal - 1;
                    }
                    else
                    {
                        nEnd = nTotal - 1;
                    }

                    // 分段响应（206 Partial Content）
                    pResp->set_status_code("206");
                    std::ostringstream oss;
                    oss << "bytes " << nStart << "-" << nEnd << "/" << nTotal;
                    pResp->add_header_pair("Content-Range", oss.str().c_str());
                    pResp->append_output_body(vecData.data() + nStart, nEnd - nStart + 1);
                    return true;
                }
            }
        }
    }

    // 无 Range / 越界：返回完整文件。
    pResp->append_output_body(vecData.data(), vecData.size());
    return true;
}

// ----------------------------------------------------------------------------
// 删除：DELETE /api/item/<id>
// ----------------------------------------------------------------------------
bool HttpHandlers::HandleDelete(WFHttpTask* pServerTask, const std::string& strId)
{
    if (s_pStore == nullptr)
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    if (!s_pStore->Remove(strId))
    {
        HttpUtil::WriteJson(pServerTask, "{\"error\":\"not found\"}", "404");
        return true;
    }
    HttpUtil::WriteJson(pServerTask, "{\"ok\":true}");
    return true;
}

}  // namespace datahub
