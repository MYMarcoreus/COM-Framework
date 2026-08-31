#pragma once

#include <cstddef>
#include <string>

#include "workflow/WFHttpServer.h"

namespace datahub {

/// @brief HTTP 工具函数（响应写入 / 请求读取 / 编码转义）。
///
/// 供 HTTP 服务与各业务处理器复用，静态函数，无状态。
class HttpUtil
{
public:
    // 写 JSON 响应。
    static void WriteJson(WFHttpTask* pServerTask, const std::string& strJson,
                          const char* szStatus = "200");

    // 写纯文本响应。
    static void WriteText(WFHttpTask* pServerTask, const std::string& strBody,
                          const char* szStatus = "200",
                          const char* szType = "text/plain; charset=utf-8");

    // 读取请求体（返回字节数，0 表示无 body）。
    static size_t ReadBody(WFHttpTask* pServerTask, std::string& strBody);

    // 读取请求头值（大小写不敏感）；不存在返回空串。
    static std::string GetHeader(WFHttpTask* pServerTask, const char* szName);

    // 获取请求来源地址（IP:port 字符串）；失败返回空串。
    static std::string PeerAddress(WFHttpTask* pServerTask);

    // URL 解码（%XX → 字符；+ → 空格）。
    static std::string UrlDecode(const std::string& strEncoded);

    // URL 编码（字符 → %XX；保留 unreserved 字符）。
    // 用于 Content-Disposition 的 RFC 5987 filename* 编码。
    static std::string UrlEncode(const std::string& strRaw);

    // 判断字符串是否含非 ASCII 字符。
    static bool HasNonAscii(const std::string& strValue);

    // HTML 转义（防 XSS）。
    static std::string HtmlEscape(const std::string& strRaw);
};

} // namespace datahub
