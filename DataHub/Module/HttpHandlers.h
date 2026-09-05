#pragma once

#include <string>

#include "Module/IDataStore.h"
#include "workflow/WFHttpServer.h"

namespace datahub {

// 前置声明（装配层持有实例）。
class CMemberService;

/// @brief HTTP 业务处理器（DataHub 业务 API）。
///
/// 实例类：构造时注入数据存储与成员服务，不再依赖静态全局状态。
/// 各方法对应一个路由，由装配层注册进 CHttpRouter。
/// 首页与静态资源由框架层（web）处理，不在本类职责内。
class CHttpHandlers
{
   public:
    // @param pStore   数据存储（IDataStore，装配层注入）
    // @param pMembers 成员服务（CMemberService，装配层注入）
    CHttpHandlers(sc::IDataStore* pStore, CMemberService* pMembers);

    // 消息列表：GET /api/list。
    bool HandleList(WFHttpTask* pServerTask);

    // 在线成员：GET /api/members。
    bool HandleMembers(WFHttpTask* pServerTask);

    // 上传文本：POST /api/text。
    bool HandleUploadText(WFHttpTask* pServerTask);

    // 获取文本：GET /api/text/<id>。
    bool HandleGetText(WFHttpTask* pServerTask, const std::string& strId);

    // 上传文件：POST /api/file。
    bool HandleUploadFile(WFHttpTask* pServerTask);

    // 下载文件 / 图片：GET /api/file/<id>。
    bool HandleGetFile(WFHttpTask* pServerTask, const std::string& strId);

    // 删除：DELETE /api/item/<id>。
    bool HandleDelete(WFHttpTask* pServerTask, const std::string& strId);

   private:
    sc::IDataStore* m_pStore;  // 数据存储（生命周期由装配层管理）
    CMemberService* m_pMembers;
};

}  // namespace datahub
