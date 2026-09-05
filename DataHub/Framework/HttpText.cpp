#include "Framework/HttpText.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace datahub {
namespace web {

std::string CHttpText::UrlDecode(const std::string& strEncoded)
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

std::string CHttpText::UrlEncode(const std::string& strRaw)
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

bool CHttpText::HasNonAscii(const std::string& strValue)
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

std::string CHttpText::HtmlEscape(const std::string& strRaw)
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

std::string CHttpText::MimeType(const std::string& strName)
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

bool CHttpText::IsImageName(const std::string& strName)
{
    return MimeType(strName).compare(0, 6, "image/") == 0;
}

bool CHttpText::ReadFile(const std::string& strPath, std::string& strOut)
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
