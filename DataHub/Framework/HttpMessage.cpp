#include "Framework/HttpMessage.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>

#include "Framework/HttpText.h"
#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"  // protocol::HttpHeaderCursor

namespace datahub {
namespace web {

// ---------------------------------------------------------------------------
// CHttpRequest
// ---------------------------------------------------------------------------

CHttpRequest::CHttpRequest(WFHttpTask* pServerTask) : m_pServerTask(pServerTask) {}

std::string CHttpRequest::Method() const
{
    return m_pServerTask->get_req()->get_method();
}

std::string CHttpRequest::Path() const
{
    std::string strUri = m_pServerTask->get_req()->get_request_uri();
    std::string::size_type nQ = strUri.find('?');
    return nQ == std::string::npos ? strUri : strUri.substr(0, nQ);
}

std::string CHttpRequest::Header(const char* szName) const
{
    protocol::HttpRequest* pReq = m_pServerTask->get_req();
    protocol::HttpHeaderCursor cursor(pReq);
    std::string strName;
    std::string strValue;
    while (cursor.next(strName, strValue))
    {
        // header 名大小写不敏感。
        std::string strLower = strName;
        std::transform(strLower.begin(), strLower.end(), strLower.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        std::string strNeed = szName;
        std::transform(strNeed.begin(), strNeed.end(), strNeed.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        if (strLower == strNeed)
        {
            return strValue;
        }
    }
    return std::string();
}

std::string CHttpRequest::QueryParam(const char* szName) const
{
    std::string strUri = m_pServerTask->get_req()->get_request_uri();
    std::string::size_type nQ = strUri.find('?');
    if (nQ == std::string::npos)
    {
        return std::string();
    }
    std::string strQuery = strUri.substr(nQ + 1);
    std::string::size_type nPos = 0;
    while (nPos <= strQuery.size())
    {
        std::string::size_type nAmp = strQuery.find('&', nPos);
        if (nAmp == std::string::npos)
        {
            nAmp = strQuery.size();
        }
        std::string strPair = strQuery.substr(nPos, nAmp - nPos);
        std::string::size_type nEq = strPair.find('=');
        std::string strKey = nEq == std::string::npos ? strPair : strPair.substr(0, nEq);
        if (strKey == szName)
        {
            return nEq == std::string::npos ? std::string() : strPair.substr(nEq + 1);
        }
        if (nAmp == strQuery.size())
        {
            break;
        }
        nPos = nAmp + 1;
    }
    return std::string();
}

std::uint64_t CHttpRequest::ContentLength() const
{
    std::string strLen = Header("Content-Length");
    if (strLen.empty())
    {
        return 0;
    }
    std::uint64_t nLen = 0;
    for (char c : strLen)
    {
        if (c < '0' || c > '9')
        {
            return 0;
        }
        nLen = nLen * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return nLen;
}

std::string CHttpRequest::Body() const
{
    protocol::HttpRequest* pReq = m_pServerTask->get_req();
    const void* pBody = nullptr;
    size_t nLen = 0;
    if (pReq->get_parsed_body(&pBody, &nLen) && nLen > 0)
    {
        return std::string(static_cast<const char*>(pBody), nLen);
    }
    return std::string();
}

std::string CHttpRequest::Peer() const
{
    struct sockaddr_storage addr;
    socklen_t nLen = sizeof(addr);
    char szAddr[64] = "unknown";
    unsigned short nPort = 0;

    if (m_pServerTask->get_peer_addr((struct sockaddr*)&addr, &nLen) == 0)
    {
        if (addr.ss_family == AF_INET)
        {
            struct sockaddr_in* pSin = (struct sockaddr_in*)&addr;
            inet_ntop(AF_INET, &pSin->sin_addr, szAddr, sizeof(szAddr));
            nPort = ntohs(pSin->sin_port);
        }
        else if (addr.ss_family == AF_INET6)
        {
            struct sockaddr_in6* pSin6 = (struct sockaddr_in6*)&addr;
            inet_ntop(AF_INET6, &pSin6->sin6_addr, szAddr, sizeof(szAddr));
            nPort = ntohs(pSin6->sin6_port);
        }
    }
    return std::string(szAddr) + ":" + std::to_string(nPort);
}

std::string CHttpRequest::PathParam() const
{
    return m_strPathParam;
}

void CHttpRequest::SetPathParam(const std::string& strParam)
{
    m_strPathParam = strParam;
}

// ---------------------------------------------------------------------------
// CHttpResponse
// ---------------------------------------------------------------------------

CHttpResponse::CHttpResponse(WFHttpTask* pServerTask) : m_pServerTask(pServerTask) {}

void CHttpResponse::WriteJson(const std::string& strJson, const char* szStatus)
{
    WriteText(strJson, szStatus, "application/json; charset=utf-8");
}

std::string CHttpResponse::StatusCode() const
{
    const char* szCode = m_pServerTask->get_resp()->get_status_code();
    return szCode != nullptr ? std::string(szCode) : std::string();
}

void CHttpResponse::WriteText(const std::string& strBody, const char* szStatus, const char* szType)
{
    protocol::HttpResponse* pResp = m_pServerTask->get_resp();
    pResp->set_status_code(szStatus);
    pResp->add_header_pair("Content-Type", szType);
    pResp->append_output_body(strBody.data(), strBody.size());
}

void CHttpResponse::AddHeader(const char* szName, const char* szValue)
{
    protocol::HttpResponse* pResp = m_pServerTask->get_resp();
    pResp->add_header_pair(szName, szValue);
}

void CHttpResponse::WriteFile(const std::string& strName, const char* pData, size_t nSize,
                              const std::string& strRangeHeader)
{
    protocol::HttpResponse* pResp = m_pServerTask->get_resp();
    pResp->set_status_code("200");

    // 图片内联（供 <img> 渲染），其他文件下载。
    if (CHttpText::IsImageName(strName))
    {
        pResp->add_header_pair("Content-Type", CHttpText::MimeType(strName).c_str());
        pResp->add_header_pair("Content-Disposition", "inline");
    }
    else
    {
        pResp->add_header_pair("Content-Type", "application/octet-stream");
        std::string strDisposition;
        if (CHttpText::HasNonAscii(strName))
        {
            strDisposition = "attachment; filename=\"download.bin\"; filename*=UTF-8''" + CHttpText::UrlEncode(strName);
        }
        else
        {
            strDisposition = "attachment; filename=\"" + strName + "\"";
        }
        pResp->add_header_pair("Content-Disposition", strDisposition.c_str());
    }

    // HTTP Range 分段支持（大文件分段传输，规避 workflow 单次超大响应截断）。
    size_t nTotal = nSize;
    size_t nStart = 0;
    size_t nEnd = nTotal > 0 ? nTotal - 1 : 0;

    if (!strRangeHeader.empty() && strRangeHeader.compare(0, 6, "bytes=") == 0 && nTotal > 0)
    {
        std::string strSpec = strRangeHeader.substr(6);
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
                    pResp->set_status_code("206");
                    std::ostringstream oss;
                    oss << "bytes " << nStart << "-" << nEnd << "/" << nTotal;
                    pResp->add_header_pair("Content-Range", oss.str().c_str());
                    pResp->append_output_body(pData + nStart, nEnd - nStart + 1);
                    return;
                }
            }
        }
    }

    // 无 Range / 越界：返回完整文件。
    pResp->append_output_body(pData, nSize);
}

}  // namespace web
}  // namespace datahub
