#pragma once

#include <cstdint>
#include <string>

#include "Module/IDataStore.h"
#include "Module/IHttpService.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "Module/ScopedInterfacePtr.h"

#include "workflow/WFHttpServer.h"

namespace datahub {

using sc::DataItemInfo;
using sc::DataKind;
using sc::IDataStore;
using sc::IHttpService;

/// @brief HTTP 数据传输服务模块（基于 Sogou Workflow）。
///
/// 封装 WFHttpServer，作为 ServerCore 模块注册：
///   - Initialize：从上下文解析 IDataStore 数据存储接口；
///   - Start：启动 HTTP 服务（路由见 IHttpService 注释）；
///   - Stop：停止 HTTP 服务。
///
/// 模块名 "http"。
class CHttpServerModule : public sc::CModule, public IHttpService
{
public:
    // 创建 HTTP 服务模块。
    // @param nPort 监听端口。
    explicit CHttpServerModule(std::uint16_t nPort);

    virtual ~CHttpServerModule();

    bool Initialize(const sc::CResolveContext& ctx) override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    std::uint16_t Port() const override;
    std::string Status() const override;

    SC_DECLARE_INTERFACE_MAP();

private:
    // 请求处理回调（WFHttpServer 调用）。
    static void ProcessRequest(WFHttpTask* pServerTask);

    // 路由分发：返回是否已写响应。
    static bool Dispatch(WFHttpTask* pServerTask, const std::string& strMethod,
                         const std::string& strPath);

    // 各路由处理（失败返回 false，由调用方写 4xx）。
    static bool HandleIndex(WFHttpTask* pServerTask);
    static bool HandleList(WFHttpTask* pServerTask);
    static bool HandleUploadText(WFHttpTask* pServerTask);
    static bool HandleGetText(WFHttpTask* pServerTask, const std::string& strId);
    static bool HandleUploadFile(WFHttpTask* pServerTask);
    static bool HandleGetFile(WFHttpTask* pServerTask, const std::string& strId);
    static bool HandleDelete(WFHttpTask* pServerTask, const std::string& strId);

    // 便捷：写 JSON 响应。
    static void WriteJson(WFHttpTask* pServerTask, const std::string& strJson,
                          const char* szStatus = "200");

    // 便捷：写纯文本响应。
    static void WriteText(WFHttpTask* pServerTask, const std::string& strBody,
                          const char* szStatus = "200", const char* szType = "text/plain; charset=utf-8");

    // 便捷：读取请求体（返回字节数，0 表示无 body）。
    static size_t ReadBody(WFHttpTask* pServerTask, std::string& strBody);

    // 便捷：读取请求头值。
    static std::string GetHeader(WFHttpTask* pServerTask, const char* szName);

    // URL 解码（%XX → 字符；+ → 空格）。
    static std::string UrlDecode(const std::string& strEncoded);

    // HTML 转义（防 XSS）。
    static std::string HtmlEscape(const std::string& strRaw);

    // 内置网页内容。
    static const char* IndexHtml();

    // 当前数据存储（Initialize 后可用）。
    static IDataStore* s_pStore;

    std::uint16_t m_nPort;
    sc::ScopedInterfacePtr<IDataStore> m_pStore;
    WFHttpServer m_server;
    bool m_bStarted;
};

} // namespace datahub
