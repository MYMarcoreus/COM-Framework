#pragma once

#include <string>

#include "Framework/HttpMessage.h"

namespace datahub {

namespace web {
class CHttpRouter;
}

/// @brief 页面 / 静态资源控制器（装配层子组件）。
///
/// 与业务控制器（CHttpHandlers）平行：首页与前端静态资源（index.html /
/// style.css / app.js）的加载、缓存与响应全部内聚在本类，装配层只负责
/// 注册路由。资源在 Initialize 时一次性读入内存（此后不再触盘），响应带
/// ETag，客户端命中 If-None-Match 时回 304 以省流量。
class CWebPageController
{
   public:
    // @param strWebDir 前端资源目录（含 index.html/style.css/app.js；
    //                 须已由装配层归一化，非空）。
    explicit CWebPageController(const std::string& strWebDir);

    // 从磁盘加载资源到内存（装配层 Initialize 调用）。
    // @return index.html 加载成功返回 true；失败时 GET / 将回 503。
    bool Load();

    // 首页是否已成功加载。
    bool IndexLoaded() const;

    // 注册本控制器负责的路由（GET /、/style.css、/app.js）。
    void RegisterRoutes(web::CHttpRouter& router);

   private:
    // 单份资源（内容 + ETag + 内容类型）。
    struct Asset
    {
        std::string strContent;
        std::string strEtag;
        std::string strType;
    };

    // 从磁盘读取一份资源并计算 ETag；缺失时保留空内容。
    bool LoadAsset(const std::string& strFile, const std::string& strMime, Asset& out);

    // 处理单份资源请求（含 304 协商）。
    // @param szMissingBody    资源缺失时的响应体
    // @param szMissingStatus  资源缺失时的状态码（"/" 用 503，静态用 404）
    bool HandleAsset(web::CHttpRequest& req, web::CHttpResponse& resp, const Asset& asset, const char* szMissingBody,
                     const char* szMissingStatus);

    // 由内容生成强 ETag（FNV-1a 哈希 + 长度）。
    static std::string MakeEtag(const std::string& strContent);

    std::string m_strWebDir;
    Asset m_index;  // GET /
    Asset m_style;  // GET /style.css
    Asset m_app;    // GET /app.js
};

}  // namespace datahub
