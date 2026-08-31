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
///   - Initialize：从上下文解析 IDataStore 数据存储接口，并加载前端页面；
///   - Start：启动 HTTP 服务（路由见 IHttpService 注释）；
///   - Stop：停止 HTTP 服务。
///
/// 前端页面为独立资源文件，构建时由 Makefile 部署到用户目录
/// `~/.datahub/index.html`，运行时从磁盘加载（路径确定，与工作目录无关）。
///
/// 模块名 "http"。
class CHttpServerModule : public sc::CModule, public IHttpService
{
   public:
    // 创建 HTTP 服务模块。
    // @param nPort     监听端口。
    // @param strIndex 前端页面文件绝对路径；空串表示使用默认用户目录
    //                `$HOME/.datahub/index.html`（由 Makefile 构建时部署）。
    explicit CHttpServerModule(std::uint16_t nPort, const std::string& strIndex = "");

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
    static bool Dispatch(WFHttpTask* pServerTask, const std::string& strMethod, const std::string& strPath);

    // 各路由处理（失败返回 false，由调用方写 4xx）。
    static bool HandleIndex(WFHttpTask* pServerTask);
    static bool HandleList(WFHttpTask* pServerTask);
    static bool HandleUploadText(WFHttpTask* pServerTask);
    static bool HandleGetText(WFHttpTask* pServerTask, const std::string& strId);
    static bool HandleUploadFile(WFHttpTask* pServerTask);
    static bool HandleGetFile(WFHttpTask* pServerTask, const std::string& strId);
    static bool HandleDelete(WFHttpTask* pServerTask, const std::string& strId);

    // 便捷：写 JSON 响应。
    static void WriteJson(WFHttpTask* pServerTask, const std::string& strJson, const char* szStatus = "200");

    // 便捷：写纯文本响应。
    static void WriteText(WFHttpTask* pServerTask, const std::string& strBody, const char* szStatus = "200",
                          const char* szType = "text/plain; charset=utf-8");

    // 便捷：读取请求体（返回字节数，0 表示无 body）。
    static size_t ReadBody(WFHttpTask* pServerTask, std::string& strBody);

    // 便捷：读取请求头值。
    static std::string GetHeader(WFHttpTask* pServerTask, const char* szName);

    // URL 解码（%XX → 字符；+ → 空格）。
    static std::string UrlDecode(const std::string& strEncoded);

    // URL 编码（字符 → %XX；保留 unreserved 字符）。用于 Content-Disposition
    // 的 RFC 5987 filename* 编码，避免 HTTP 头出现非 ASCII 字节。
    static std::string UrlEncode(const std::string& strRaw);

    // 判断字符串是否含非 ASCII 字符。
    static bool HasNonAscii(const std::string& strValue);

    // HTML 转义（防 XSS）。
    static std::string HtmlEscape(const std::string& strRaw);

    // 从磁盘加载前端页面文件（路径 m_strIndexPath 由配置 [web] index 指定）；成功返回 true。
    bool LoadIndexHtml();

    // 当前数据存储（Initialize 后可用）。
    static IDataStore* s_pStore;
    // 已加载的前端页面内容（静态指针，供静态回调访问；Initialize 时指向实例成员）。
    static const std::string* s_pIndexHtml;

    std::uint16_t m_nPort;
    std::string m_strIndexPath;  // 前端页面文件路径
    std::string m_strIndexHtml;  // 已加载的前端页面内容（空表示加载失败）
    sc::ScopedInterfacePtr<IDataStore> m_pStore;
    WFHttpServer m_server;
    bool m_bStarted;
};

}  // namespace datahub
