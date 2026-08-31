#pragma once

#include <string>

#include "Module/IDataStore.h"
#include "workflow/WFHttpServer.h"

namespace datahub {

/// @brief HTTP 业务处理器。
///
/// 实现各 API 路由的具体处理（列表 / 文本 / 文件 / 成员 / 删除 / 首页）。
/// 通过静态指针访问数据存储与前端页面（由 HttpServerModule 在 Initialize 时设置）。
class HttpHandlers
{
   public:
    // 设置数据存储与前端页面（HttpServerModule::Initialize 调用）。
    static void SetStore(sc::IDataStore* pStore);
    static void SetIndexHtml(const std::string* pIndexHtml);

    // 首页：返回前端页面。
    static bool HandleIndex(WFHttpTask* pServerTask);

    // 静态资源：GET /style.css、/app.js（前端页面引用的独立资源）。
    static bool HandleStatic(WFHttpTask* pServerTask, const std::string& strName);

    // 消息列表：GET /api/list。
    static bool HandleList(WFHttpTask* pServerTask);

    // 在线成员：GET /api/members。
    static bool HandleMembers(WFHttpTask* pServerTask);

    // 上传文本：POST /api/text。
    static bool HandleUploadText(WFHttpTask* pServerTask);

    // 获取文本：GET /api/text/<id>。
    static bool HandleGetText(WFHttpTask* pServerTask, const std::string& strId);

    // 上传文件：POST /api/file。
    static bool HandleUploadFile(WFHttpTask* pServerTask);

    // 下载文件 / 图片：GET /api/file/<id>。
    static bool HandleGetFile(WFHttpTask* pServerTask, const std::string& strId);

    // 删除：DELETE /api/item/<id>。
    static bool HandleDelete(WFHttpTask* pServerTask, const std::string& strId);

   private:
    static sc::IDataStore* s_pStore;
    static const std::string* s_pIndexHtml;
};

}  // namespace datahub
