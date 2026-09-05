#pragma once

#include <string>

#include "Framework/HttpMessage.h"
#include "Module/IDataStore.h"

namespace datahub {

// 前置声明（装配层持有实例）。
class CMemberService;

/// @brief HTTP 业务处理器（DataHub 业务 API）。
///
/// 实例类：构造时注入数据存储与成员服务，不依赖静态全局状态。
/// 各方法对应一个路由，由装配层注册进 CHttpRouter；签名使用框架的
/// CHttpRequest / CHttpResponse，不直接接触 workflow 类型。
/// 首页与静态资源由装配层处理，不在本类职责内。
class CHttpHandlers
{
   public:
    // @param pStore   数据存储（IDataStore，装配层注入）
    // @param pMembers 成员服务（CMemberService，装配层注入）
    CHttpHandlers(sc::IDataStore* pStore, CMemberService* pMembers);

    // 消息列表：GET /api/list。
    bool HandleList(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 在线成员：GET /api/members。
    bool HandleMembers(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 上传文本：POST /api/text（body 为内容）。
    bool HandleUploadText(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 获取文本：GET /api/text/<id>（id 取自 req.PathParam()）。
    bool HandleGetText(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 上传文件：POST /api/file（header X-File-Name 指定文件名）。
    bool HandleUploadFile(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 下载文件 / 图片：GET /api/file/<id>。
    bool HandleGetFile(web::CHttpRequest& req, web::CHttpResponse& resp);

    // 删除：DELETE /api/item/<id>。
    bool HandleDelete(web::CHttpRequest& req, web::CHttpResponse& resp);

   private:
    sc::IDataStore* m_pStore;  // 数据存储（生命周期由装配层管理）
    CMemberService* m_pMembers;
};

}  // namespace datahub
