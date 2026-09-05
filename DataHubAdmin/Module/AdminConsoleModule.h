#pragma once

#include <cstdint>
#include <string>

#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "workflow/WFHttpServer.h"

namespace datahubadmin {

/// @brief 管理控制台服务模块（独立服务器：控制面）。
///
/// 职责：
///   - 承载服务端管理网页（GET /：租户一览/成员/数据条目/配额/删除管理）；
///   - 把 /api/admin/* 请求反向代理到上游 DataHub 数据面（本机回环地址），
///     并自动附加 X-Admin-Token（令牌只在服务器配置里，不进入浏览器）；
///   - 仅允许本机回环来源访问（管理面不暴露给局域网设备）。
///
/// 进程间不共享内存：本模块不持有租户/数据，全部经上游 API 操作，天然与
/// DataHub 解耦。模块名 "adminconsole"。
class CAdminConsoleModule : public sc::CModule
{
   public:
    // @param nPort          控制台监听端口
    // @param strUpstream    DataHub 数据面基址（如 http://127.0.0.1:8888）
    // @param strToken       X-Admin-Token（须与 DataHub [admin] token 一致）
    // @param strWebDir      管理网页目录（空 = 默认 $HOME/.datahub-admin）
    CAdminConsoleModule(std::uint16_t nPort, const std::string& strUpstream, const std::string& strToken,
                        const std::string& strWebDir = "");

    virtual ~CAdminConsoleModule();

    bool Initialize(const sc::CResolveContext& ctx) override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    SC_DECLARE_INTERFACE_MAP();

   private:
    // 请求处理回调（Workflow 线程池）。
    void OnRequest(WFHttpTask* pServerTask);

    // 处理静态管理页（/、/admin.js）。
    void ServePage(WFHttpTask* pServerTask, const std::string& strPath);

    // 代理 /api/admin/* 到上游 DataHub。
    void ProxyToUpstream(WFHttpTask* pServerTask, const std::string& strUri);

    // 上游响应回调：把 DataHub 响应搬到控制台响应。
    void OnUpstreamReply(WFHttpTask* pClientTask, WFHttpTask* pServerTask);

    std::uint16_t m_nPort;
    std::string m_strUpstream;   // 上游 DataHub 基址（无结尾 '/'）
    std::string m_strToken;      // X-Admin-Token
    std::string m_strWebDir;     // 管理网页目录
    std::string m_indexHtml;     // GET /
    std::string m_adminJs;       // GET /admin.js
    WFHttpServer m_server;
    bool m_bStarted;
};

}  // namespace datahubadmin
