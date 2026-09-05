#include "Framework/HttpUtil.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"

namespace datahub {
namespace web {

void CHttpUtil::WriteJson(WFHttpTask* pServerTask, const std::string& strJson, const char* szStatus)
{
    WriteText(pServerTask, strJson, szStatus, "application/json; charset=utf-8");
}

void CHttpUtil::WriteText(WFHttpTask* pServerTask, const std::string& strBody, const char* szStatus, const char* szType)
{
    protocol::HttpResponse* pResp = pServerTask->get_resp();
    pResp->set_status_code(szStatus);
    pResp->add_header_pair("Content-Type", szType);
    pResp->append_output_body(strBody.data(), strBody.size());
}

size_t CHttpUtil::ReadBody(WFHttpTask* pServerTask, std::string& strBody)
{
    protocol::HttpRequest* pReq = pServerTask->get_req();
    const void* pBody = nullptr;
    size_t nLen = 0;
    if (pReq->get_parsed_body(&pBody, &nLen) && nLen > 0)
    {
        strBody.assign(static_cast<const char*>(pBody), nLen);
        return nLen;
    }
    strBody.clear();
    return 0;
}

std::string CHttpUtil::GetHeader(WFHttpTask* pServerTask, const char* szName)
{
    protocol::HttpRequest* pReq = pServerTask->get_req();
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

std::string CHttpUtil::PeerAddress(WFHttpTask* pServerTask)
{
    struct sockaddr_storage addr;
    socklen_t nLen = sizeof(addr);
    char szAddr[64] = "unknown";
    unsigned short nPort = 0;

    if (pServerTask->get_peer_addr((struct sockaddr*)&addr, &nLen) == 0)
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

std::string CHttpUtil::UrlDecode(const std::string& strEncoded)
{
    std::string strOut;
    strOut.reserve(strEncoded.size());
    for (size_t i = 0; i < strEncoded.size(); ++i)
    {
        if (strEncoded[i] == '%' && i + 2 < strEncoded.size())
        {
            int nHigh = 0, nLow = 0;
            char c1 = strEncoded[i + 1];
            char c2 = strEncoded[i + 2];
            if (c1 >= '0' && c1 <= '9')
                nHigh = c1 - '0';
            else if (c1 >= 'a' && c1 <= 'f')
                nHigh = c1 - 'a' + 10;
            else if (c1 >= 'A' && c1 <= 'F')
                nHigh = c1 - 'A' + 10;
            else
            {
                strOut.push_back(strEncoded[i]);
                continue;
            }
            if (c2 >= '0' && c2 <= '9')
                nLow = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f')
                nLow = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F')
                nLow = c2 - 'A' + 10;
            else
            {
                strOut.push_back(strEncoded[i]);
                continue;
            }
            strOut.push_back(static_cast<char>((nHigh << 4) | nLow));
            i += 2;
        }
        else if (strEncoded[i] == '+')
        {
            strOut.push_back(' ');
        }
        else
        {
            strOut.push_back(strEncoded[i]);
        }
    }
    return strOut;
}

std::string CHttpUtil::UrlEncode(const std::string& strRaw)
{
    static const char* const kHex = "0123456789ABCDEF";
    std::string strOut;
    strOut.reserve(strRaw.size() * 3);
    for (unsigned char c : strRaw)
    {
        // unreserved: A-Z a-z 0-9 - _ . ~
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
        {
            strOut.push_back(static_cast<char>(c));
        }
        else
        {
            strOut.push_back('%');
            strOut.push_back(kHex[(c >> 4) & 0x0F]);
            strOut.push_back(kHex[c & 0x0F]);
        }
    }
    return strOut;
}

bool CHttpUtil::HasNonAscii(const std::string& strValue)
{
    for (unsigned char c : strValue)
    {
        if (c >= 0x80)
        {
            return true;
        }
    }
    return false;
}

std::string CHttpUtil::HtmlEscape(const std::string& strRaw)
{
    std::string strOut;
    strOut.reserve(strRaw.size());
    for (char c : strRaw)
    {
        switch (c)
        {
            case '&':
                strOut += "&amp;";
                break;
            case '<':
                strOut += "&lt;";
                break;
            case '>':
                strOut += "&gt;";
                break;
            case '"':
                strOut += "&quot;";
                break;
            case '\'':
                strOut += "&#39;";
                break;
            default:
                strOut.push_back(c);
                break;
        }
    }
    return strOut;
}

std::string CHttpUtil::MimeType(const std::string& strName)
{
    std::string strLower = strName;
    std::transform(strLower.begin(), strLower.end(), strLower.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    std::size_t nLen = strLower.size();

    auto EndsWith = [&](const char* szSuffix, std::size_t nSuffix) {
        return nLen >= nSuffix && strLower.compare(nLen - nSuffix, nSuffix, szSuffix) == 0;
    };

    if (EndsWith(".png", 4)) return "image/png";
    if (EndsWith(".jpg", 4) || EndsWith(".jpeg", 5)) return "image/jpeg";
    if (EndsWith(".gif", 4)) return "image/gif";
    if (EndsWith(".webp", 5)) return "image/webp";
    if (EndsWith(".bmp", 4)) return "image/bmp";
    if (EndsWith(".svg", 4)) return "image/svg+xml";
    if (EndsWith(".ico", 4)) return "image/x-icon";
    if (EndsWith(".txt", 4) || EndsWith(".log", 4)) return "text/plain";
    if (EndsWith(".html", 5) || EndsWith(".htm", 4)) return "text/html";
    if (EndsWith(".css", 4)) return "text/css";
    if (EndsWith(".js", 3)) return "text/javascript";
    if (EndsWith(".json", 5)) return "application/json";
    if (EndsWith(".pdf", 4)) return "application/pdf";
    if (EndsWith(".zip", 4)) return "application/zip";
    return "application/octet-stream";
}

bool CHttpUtil::IsImageName(const std::string& strName)
{
    std::string strMime = MimeType(strName);
    return strMime.compare(0, 6, "image/") == 0;
}

void CHttpUtil::WriteFile(WFHttpTask* pServerTask, const std::string& strName, const char* pData, size_t nSize,
                          const std::string& strRangeHeader)
{
    protocol::HttpResponse* pResp = pServerTask->get_resp();
    pResp->set_status_code("200");

    // 图片内联（供 <img> 渲染），其他文件下载。
    if (IsImageName(strName))
    {
        pResp->add_header_pair("Content-Type", MimeType(strName).c_str());
        pResp->add_header_pair("Content-Disposition", "inline");
    }
    else
    {
        pResp->add_header_pair("Content-Type", "application/octet-stream");
        std::string strDisposition;
        if (HasNonAscii(strName))
        {
            strDisposition = "attachment; filename=\"download.bin\"; filename*=UTF-8''" + UrlEncode(strName);
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

bool CHttpUtil::ReadFile(const std::string& strPath, std::string& strOut)
{
    std::ifstream ifs(strPath.c_str(), std::ios::binary);
    if (!ifs.is_open())
    {
        return false;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    strOut = ss.str();
    return true;
}

}  // namespace web
}  // namespace datahub
