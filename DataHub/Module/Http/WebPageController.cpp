#include "Module/Http/WebPageController.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "Framework/HttpRouter.h"
#include "Framework/HttpText.h"
#include "Log/Logger.h"

namespace datahub {

/// @brief 创建页面/静态资源控制器。
CWebPageController::CWebPageController(const std::string& strWebDir) : m_strWebDir(strWebDir) {}

/// @brief 从磁盘加载全部前端资源到内存（一次性；之后请求不再触盘）。
bool CWebPageController::Load()
{
    // index.html：首页内容；缺失视为致命（GET / 回 503）。
    if (!LoadAsset("index.html", "text/html; charset=utf-8", m_index))
    {
        common::log::CLogger::Instance().Warn("[DataHub] 前端 index.html 加载失败: " + m_strWebDir +
                                              "/index.html（GET / 将返回 503）");
        return false;
    }
    // 静态资源：尽力加载；单份缺失时对应路由回 404。
    LoadAsset("style.css", "text/css; charset=utf-8", m_style);
    LoadAsset("app.js", "text/javascript; charset=utf-8", m_app);
    return true;
}

bool CWebPageController::IndexLoaded() const
{
    return !m_index.strContent.empty();
}

/// @brief 读取一份资源文件并计算 ETag。
bool CWebPageController::LoadAsset(const std::string& strFile, const std::string& strMime, Asset& out)
{
    std::string strContent;
    if (!web::CHttpText::ReadFile(m_strWebDir + "/" + strFile, strContent))
    {
        return false;
    }
    out.strContent = strContent;
    out.strEtag = MakeEtag(strContent);
    out.strType = strMime;
    return true;
}

/// @brief 注册 GET /、/style.css、/app.js。
void CWebPageController::RegisterRoutes(web::CHttpRouter& router)
{
    router.Register({"GET", "/", [this](web::CHttpRequest& req, web::CHttpResponse& resp)
    { return HandleAsset(req, resp, m_index, "前端页面未加载", "503"); }});
    router.Register({"GET", "/style.css", [this](web::CHttpRequest& req, web::CHttpResponse& resp)
    { return HandleAsset(req, resp, m_style, "not found", "404"); }});
    router.Register({"GET", "/app.js", [this](web::CHttpRequest& req, web::CHttpResponse& resp)
    { return HandleAsset(req, resp, m_app, "not found", "404"); }});
}

/// @brief 处理单份资源：缺失回缺省状态码；命中 If-None-Match 回 304；否则 200 全文。
bool CWebPageController::HandleAsset(web::CHttpRequest& req, web::CHttpResponse& resp, const Asset& asset,
                                     const char* szMissingBody, const char* szMissingStatus)
{
    if (asset.strContent.empty())
    {
        resp.WriteText(szMissingBody, szMissingStatus, "text/plain");
        return true;
    }
    resp.AddHeader("ETag", asset.strEtag.c_str());
    // 强缓存：命中 ETag 则 304，节省重复传输。
    std::string strInm = req.Header("If-None-Match");
    if (!strInm.empty() && strInm == asset.strEtag)
    {
        resp.WriteText("", "304", asset.strType.c_str());
        return true;
    }
    resp.WriteText(asset.strContent, "200", asset.strType.c_str());
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
