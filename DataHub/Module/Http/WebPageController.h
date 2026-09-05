#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "Framework/HttpMessage.h"

namespace datahub {

namespace web {
class CHttpRouter;
}

/// @brief 页面 / 静态资源控制器（装配层子组件）。
///
/// 与业务控制器（CHttpHandlers / CTenantsController）平行：页面与前端静态
/// 资源（index.html / style.css / app.js / common.js / tenants.html /
/// tenants.js）的加载、缓存与响应全部内聚在本类，装配层只负责注册路由。
/// 路由→资源以 m_pages 表驱动：新增页面只需在构造函数表加一项。资源在
/// Initialize 时一次性读入内存（此后不再触盘），响应带 ETag，客户端命中
/// If-None-Match 时回 304 以省流量。
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

    // 注册本控制器负责的全部路由（按 m_pages 表：/ 租户管理页、/chat 聊天室与静态资源）。
    void RegisterRoutes(web::CHttpRouter& router);

   private:
    // 单份资源（内容 + ETag + 内容类型）。
    struct Asset
    {
        std::string strContent;
        std::string strEtag;
        std::string strType;
    };

    // 一份可注册资源：路由描述 + 已加载内容（页面 / 静态资源共用）。
    struct Page
    {
        const char* szRoute;          // 路由（如 "/"、"/chat"）
        const char* szFile;           // 磁盘文件名（webDir 下）
        const char* szMime;           // Content-Type
        const char* szMissingBody;    // 资源缺失时的响应体
        const char* szMissingStatus;  // 资源缺失时的状态码（首页 503，其余 404）
        Asset asset;
    };

    // 读取一份资源文件并计算 ETag；缺失时保留空内容。
    bool LoadAsset(Page& page);

    // 处理单份资源请求（含 304 协商）。
    bool HandleAsset(web::CHttpRequest& req, web::CHttpResponse& resp, const Page& page);

    // 由内容生成强 ETag（FNV-1a 哈希 + 长度）。
    static std::string MakeEtag(const std::string& strContent);

    std::string m_strWebDir;
    std::vector<Page> m_pages;  // 路由→资源表（构造函数填满；注册后元素地址稳定）
};

}  // namespace datahub
