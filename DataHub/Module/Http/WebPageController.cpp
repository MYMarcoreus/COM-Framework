#include "Module/Http/WebPageController.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "Framework/HttpRouter.h"
#include "Framework/HttpText.h"
#include "Log/Logger.h"

namespace datahub {

/// @brief 创建页面/静态资源控制器；初始化路由→资源表。
///
/// 新增前端页面或静态资源只需在此表加一行：Load 会按表加载，
/// RegisterRoutes 会按表注册。首页（"/"）缺失视为致命（回 503），
/// 其余静态资源缺失回 404。
CWebPageController::CWebPageController(const std::string& strWebDir) : m_strWebDir(strWebDir)
{
    static const Page kPages[] = {
        {"/", "tenants.html", "text/html; charset=utf-8", "前端页面未加载", "503", {}},
        {"/chat", "index.html", "text/html; charset=utf-8", "前端页面未加载", "503", {}},
        {"/style.css", "style.css", "text/css; charset=utf-8", "not found", "404", {}},
        {"/app.js", "app.js", "text/javascript; charset=utf-8", "not found", "404", {}},
        {"/common.js", "common.js", "text/javascript; charset=utf-8", "not found", "404", {}},
        {"/tenants.js", "tenants.js", "text/javascript; charset=utf-8", "not found", "404", {}},
    };
    m_pages.assign(kPages, kPages + sizeof(kPages) / sizeof(kPages[0]));
}

/// @brief 从磁盘加载全部前端资源到内存（一次性；之后请求不再触盘）。
bool CWebPageController::Load()
{
    for (std::size_t i = 0; i < m_pages.size(); ++i)
    {
        if (LoadAsset(m_pages[i]))
        {
            continue;
        }
        // 根页缺失视为致命：GET /（租户管理页）回 503；其余尽力加载（缺失回对应状态）。
        if (std::string(m_pages[i].szRoute) == "/")
        {
            common::log::CLogger::Instance().Warn("[DataHub] 前端根页 tenants.html 加载失败: " + m_strWebDir +
                                                  "/tenants.html（GET / 将返回 503）");
        }
    }
    return IndexLoaded();
}

bool CWebPageController::IndexLoaded() const
{
    for (std::size_t i = 0; i < m_pages.size(); ++i)
    {
        if (std::string(m_pages[i].szRoute) == "/")
        {
            return !m_pages[i].asset.strContent.empty();
        }
    }
    return false;
}

/// @brief 读取一份资源文件并计算 ETag；缺失时保留空内容。
bool CWebPageController::LoadAsset(Page& page)
{
    std::string strContent;
    if (!web::CHttpText::ReadFile(m_strWebDir + "/" + page.szFile, strContent))
    {
        return false;
    }
    page.asset.strContent = strContent;
    page.asset.strEtag = MakeEtag(strContent);
    page.asset.strType = page.szMime;
    return true;
}

/// @brief 注册全部路由（按 m_pages 表）。
/// 捕获元素下标而非迭代器/引用，避免 lambda 持有悬垂指针。
void CWebPageController::RegisterRoutes(web::CHttpRouter& router)
{
    for (std::size_t i = 0; i < m_pages.size(); ++i)
    {
        router.Register({"GET", m_pages[i].szRoute, [this, i](web::CHttpRequest& req, web::CHttpResponse& resp)
        { return HandleAsset(req, resp, m_pages[i]); }});
    }
}

/// @brief 处理单份资源：缺失回缺省状态码；命中 If-None-Match 回 304；否则 200 全文。
bool CWebPageController::HandleAsset(web::CHttpRequest& req, web::CHttpResponse& resp, const Page& page)
{
    if (page.asset.strContent.empty())
    {
        resp.WriteText(page.szMissingBody, page.szMissingStatus, "text/plain");
        return true;
    }
    resp.AddHeader("ETag", page.asset.strEtag.c_str());
    // 强缓存：命中 ETag 则 304，节省重复传输。
    std::string strInm = req.Header("If-None-Match");
    if (!strInm.empty() && strInm == page.asset.strEtag)
    {
        resp.WriteText("", "304", page.asset.strType.c_str());
        return true;
    }
    resp.WriteText(page.asset.strContent, "200", page.asset.strType.c_str());
    return true;
}

/// @brief 由内容生成强 ETag："hash-len"。
std::string CWebPageController::MakeEtag(const std::string& strContent)
{
    std::uint64_t nHash = 1469598103934665603ULL;  // FNV-1a 偏移
    for (unsigned char c : strContent)
    {
        nHash ^= c;
        nHash *= 1099511628211ULL;
    }
    char szBuf[64];
    std::snprintf(szBuf, sizeof(szBuf), "\"%016llx-%zu\"", static_cast<unsigned long long>(nHash), strContent.size());
    return std::string(szBuf);
}

}  // namespace datahub
