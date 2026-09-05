#pragma once

#include <cstddef>
#include <string>

#include "workflow/WFHttpServer.h"

namespace datahub {
namespace web {

/// @brief HTTP 请求封装（只读侧）。
///
/// 薄封装 WFHttpTask 的请求部分，向业务层提供方法/路径/头/体/来源等只读视图，
/// 使业务层不直接接触 workflow 类型。前缀路由命中时由 CHttpRouter 填充
/// PathParam()（匹配前缀后的剩余路径段，如 /api/text/<id> 的 <id>）。
class CHttpRequest
{
   public:
    // 以请求任务构造。
    explicit CHttpRequest(WFHttpTask* pServerTask);

    // 请求方法（"GET"/"POST"/...）。
    std::string Method() const;

    // 完整请求路径（不含 query）。
    std::string Path() const;

    // 读取请求头值（大小写不敏感）；不存在返回空串。
    std::string Header(const char* szName) const;

    // 读取请求体内容（空表示无 body）。
    std::string Body() const;

    // 请求来源地址（IP:port）。
    std::string Peer() const;

    // 前缀路由的剩余路径段（exact 路由为空）。由 Router 分发时填充。
    std::string PathParam() const;
    void SetPathParam(const std::string& strParam);

   private:
    WFHttpTask* m_pServerTask;
    std::string m_strPathParam;
};

/// @brief HTTP 响应封装（只写侧）。
///
/// 薄封装 WFHttpTask 的响应部分：写 JSON/文本/文件响应。文件响应含图片内联 /
/// 附件下载与 Range 分段。业务层通过本对象写响应，不直接接触 workflow 类型。
class CHttpResponse
{
   public:
    // 以请求任务构造（同一任务内请求与响应成对）。
    explicit CHttpResponse(WFHttpTask* pServerTask);

    // 写 JSON 响应。
    void WriteJson(const std::string& strJson, const char* szStatus = "200");

    // 写纯文本响应。
    void WriteText(const std::string& strBody, const char* szStatus = "200",
                   const char* szType = "text/plain; charset=utf-8");

    // 写文件响应：图片内联或附件下载，并支持 HTTP Range 分段（206）。
    // @param strName         文件名（决定 Content-Type / Content-Disposition）
    // @param pData / nSize   文件内容
    // @param strRangeHeader  请求的 Range 头（空表示完整返回）
    void WriteFile(const std::string& strName, const char* pData, size_t nSize, const std::string& strRangeHeader);

   private:
    WFHttpTask* m_pServerTask;
};

}  // namespace web
}  // namespace datahub
