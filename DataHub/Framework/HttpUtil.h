#pragma once

#include <cstddef>
#include <string>

#include "workflow/WFHttpServer.h"

namespace datahub {
namespace web {

/// @brief HTTP 工具函数（响应写入 / 请求读取 / 编码转义）。
///
/// 通用 Web 框架工具（无业务依赖），静态函数，无状态。
/// 响应写入与请求解析基于 workflow 消息对象。
class CHttpUtil
{
   public:
    // 写 JSON 响应。
    static void WriteJson(WFHttpTask* pServerTask, const std::string& strJson, const char* szStatus = "200");

    // 写纯文本响应。
    static void WriteText(WFHttpTask* pServerTask, const std::string& strBody, const char* szStatus = "200",
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

    // 依据文件名后缀返回 MIME 类型；未知返回 "application/octet-stream"。
    static std::string MimeType(const std::string& strName);

    // 判断是否为浏览器可内联显示的图片（png/jpg/jpeg/gif/webp/bmp/svg）。
    static bool IsImageName(const std::string& strName);

    // 写文件响应：按类型返回内联（图片，供 <img> 渲染）或附件下载（RFC 5987
    // 中文名编码），并支持 HTTP Range 分段（206 Partial Content）。
    // @param strName         文件名（决定 Content-Type / Content-Disposition）
    // @param pData / nSize   文件内容
    // @param strRangeHeader  请求的 Range 头（空表示完整返回）
    static void WriteFile(WFHttpTask* pServerTask, const std::string& strName, const char* pData, size_t nSize,
                          const std::string& strRangeHeader);

    // 从磁盘读取整个文件到字符串；成功返回 true。
    static bool ReadFile(const std::string& strPath, std::string& strOut);
};

}  // namespace web
}  // namespace datahub
