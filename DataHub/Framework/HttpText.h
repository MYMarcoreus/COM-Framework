#pragma once

#include <string>

namespace datahub {
namespace web {

/// @brief HTTP 文本工具（纯函数，无 workflow 依赖）。
///
/// URL / HTML 编解码、非 ASCII 判断、磁盘文件读取、MIME 类型判定。
class CHttpText
{
   public:
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

    // 从磁盘读取整个文件到字符串；成功返回 true。
    static bool ReadFile(const std::string& strPath, std::string& strOut);
};

}  // namespace web
}  // namespace datahub
