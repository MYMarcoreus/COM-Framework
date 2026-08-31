#include "Module/HttpServerModule.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "Log/Logger.h"
#include "Module/InterfaceMap.h"
#include "Module/ResolveContext.h"
#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"

namespace datahub {

IDataStore* CHttpServerModule::s_pStore = nullptr;
const std::string* CHttpServerModule::s_pIndexHtml = nullptr;

/// @brief 创建 HTTP 服务模块。
CHttpServerModule::CHttpServerModule(std::uint16_t nPort, const std::string& strIndex)
    : sc::CModule("http"),
      m_nPort(nPort),
      m_strIndexPath(strIndex),
      m_server(&CHttpServerModule::ProcessRequest),
      m_bStarted(false)
{
    // 依赖 IDataStore 接口模块：生命周期拓扑排序保证其先初始化 / 启动。
    AddDependency(sc::IID_IDataStore());
}

/// @brief 销毁 HTTP 服务模块。
CHttpServerModule::~CHttpServerModule()
{
    Stop();
}

/// @brief 从初始化上下文解析数据存储接口，并加载前端页面。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true 数据存储接口就绪；false 缺失。
bool CHttpServerModule::Initialize(const sc::CResolveContext& ctx)
{
    m_pStore.Reset(ctx.Resolve<IDataStore>());
    if (m_pStore == nullptr)
    {
        return false;
    }
    // 回调为静态方法，通过静态指针访问数据存储。
    s_pStore = m_pStore.Get();

    // 加载前端页面（独立资源文件）；失败仅告警，不影响服务启动。
    if (!LoadIndexHtml())
    {
        common::log::CLogger::Instance().Warn("[DataHub] 前端页面加载失败: " + m_strIndexPath + "（GET / 将返回 503）");
    }
    // 回调为静态方法，通过静态指针访问页面内容。
    s_pIndexHtml = &m_strIndexHtml;
    return true;
}

/// @brief 从磁盘加载前端页面文件。
///
/// 路径解析：
///   - 配置 [web] index 指定（m_strIndexPath，绝对路径）；否则
///   - 默认用户目录 `$HOME/.datahub/index.html`（构建时由 Makefile 部署）。
/// 直接读取，无自动推导，与进程工作目录无关。
///
/// @return true 加载成功；false 文件不存在或读取失败。
bool CHttpServerModule::LoadIndexHtml()
{
    std::string strPath = m_strIndexPath;
    if (strPath.empty())
    {
        const char* szHome = ::getenv("HOME");
        if (szHome == nullptr)
        {
            return false;
        }
        strPath = std::string(szHome) + "/.datahub/index.html";
    }

    std::ifstream ifs(strPath.c_str(), std::ios::binary);
    if (!ifs.is_open())
    {
        return false;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    m_strIndexHtml = ss.str();
    return !m_strIndexHtml.empty();
}

/// @brief 启动 HTTP 服务。
///
/// @return true 启动成功；false 端口被占用等。
bool CHttpServerModule::Start()
{
    if (m_bStarted)
    {
        return true;
    }
    if (m_server.start(m_nPort) == 0)
    {
        m_bStarted = true;
        common::log::CLogger::Instance().Info("[DataHub] HTTP 服务已启动，监听端口 " + std::to_string(m_nPort));
        return true;
    }
    return false;
}

/// @brief 停止 HTTP 服务。
void CHttpServerModule::Stop()
{
    if (m_bStarted)
    {
        m_server.stop();
        m_bStarted = false;
        common::log::CLogger::Instance().Info("[DataHub] HTTP 服务已停止");
    }
}

void CHttpServerModule::Shutdown()
{
    Stop();
    m_pStore.Reset();
    s_pStore = nullptr;
    s_pIndexHtml = nullptr;
}

std::uint16_t CHttpServerModule::Port() const
{
    return m_nPort;
}

std::string CHttpServerModule::Status() const
{
    return "http:port=" + std::to_string(m_nPort) + " started=" + (m_bStarted ? "1" : "0");
}

// ----------------------------------------------------------------------------
// 请求处理回调（Workflow 线程池中执行）
// ----------------------------------------------------------------------------
void CHttpServerModule::ProcessRequest(WFHttpTask* pServerTask)
{
    protocol::HttpRequest* pReq = pServerTask->get_req();
    protocol::HttpResponse* pResp = pServerTask->get_resp();

    std::string strMethod = pReq->get_method();
    std::string strPath = pReq->get_request_uri();
    // 去掉 query 部分（? 之后）。
    std::string::size_type nQ = strPath.find('?');
    if (nQ != std::string::npos)
    {
        strPath = strPath.substr(0, nQ);
    }

    if (!Dispatch(pServerTask, strMethod, strPath))
    {
        WriteText(pServerTask, "Not Found", "404", "text/plain");
    }

    // 统一响应头。
    pResp->add_header_pair("Server", "DataHub/1.0");
}

// ----------------------------------------------------------------------------
// 路由分发
// ----------------------------------------------------------------------------
bool CHttpServerModule::Dispatch(WFHttpTask* pServerTask, const std::string& strMethod, const std::string& strPath)
{
    // 首页（GET /）
    if (strMethod == "GET" && strPath == "/")
    {
        return HandleIndex(pServerTask);
    }
    // 列表（GET /api/list）
    if (strMethod == "GET" && strPath == "/api/list")
    {
        return HandleList(pServerTask);
    }
    // 上传文本（POST /api/text）
    if (strMethod == "POST" && strPath == "/api/text")
    {
        return HandleUploadText(pServerTask);
    }
    // 上传文件（POST /api/file）
    if (strMethod == "POST" && strPath == "/api/file")
    {
        return HandleUploadFile(pServerTask);
    }
    // GET /api/text/<id>、GET /api/file/<id>、DELETE /api/item/<id>
    // 注意前缀 "/api/text/" 长度为 10（/api/ 5 + text/ 5）。
    if (strMethod == "GET" && strPath.compare(0, 10, "/api/text/") == 0)
    {
        return HandleGetText(pServerTask, UrlDecode(strPath.substr(10)));
    }
    if (strMethod == "GET" && strPath.compare(0, 10, "/api/file/") == 0)
    {
        return HandleGetFile(pServerTask, UrlDecode(strPath.substr(10)));
    }
    if (strMethod == "DELETE" && strPath.compare(0, 10, "/api/item/") == 0)
    {
        return HandleDelete(pServerTask, UrlDecode(strPath.substr(10)));
    }
    return false;
}

// ----------------------------------------------------------------------------
// 首页：返回前端页面（独立资源文件 Web/index.html，Initialize 时加载）
// ----------------------------------------------------------------------------
bool CHttpServerModule::HandleIndex(WFHttpTask* pServerTask)
{
    protocol::HttpResponse* pResp = pServerTask->get_resp();
    if (s_pIndexHtml == nullptr || s_pIndexHtml->empty())
    {
        pResp->set_status_code("503");
        pResp->add_header_pair("Content-Type", "text/plain; charset=utf-8");
        pResp->append_output_body("前端页面未加载");
        return true;
    }
    pResp->set_status_code("200");
    pResp->add_header_pair("Content-Type", "text/html; charset=utf-8");
    pResp->append_output_body(s_pIndexHtml->data(), s_pIndexHtml->size());
    return true;
}

// ----------------------------------------------------------------------------
// 列表：GET /api/list —— 返回 JSON 数组
// ----------------------------------------------------------------------------
bool CHttpServerModule::HandleList(WFHttpTask* pServerTask)
{
    if (s_pStore == nullptr)
    {
        WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::vector<DataItemInfo> vecItems = s_pStore->List();
    std::ostringstream oss;
    oss << "{\"items\":[";
    bool bFirst = true;
    for (const DataItemInfo& info : vecItems)
    {
        if (!bFirst)
        {
            oss << ",";
        }
        bFirst = false;
        oss << "{\"id\":\"" << info.strId << "\"" << ",\"type\":\"" << (info.kind == DataKind::kText ? "text" : "file")
            << "\"" << ",\"name\":\"" << HtmlEscape(info.strName) << "\"" << ",\"size\":" << info.nSize
            << ",\"time\":" << info.nCreateMs << "}";
    }
    oss << "]}";
    WriteJson(pServerTask, oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 上传文本：POST /api/text —— body 为文本内容
// ----------------------------------------------------------------------------
bool CHttpServerModule::HandleUploadText(WFHttpTask* pServerTask)
{
    if (s_pStore == nullptr)
    {
        WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strBody;
    ReadBody(pServerTask, strBody);
    if (strBody.empty())
    {
        WriteJson(pServerTask, "{\"error\":\"empty body\"}", "400");
        return true;
    }
    std::string strId = s_pStore->SaveText(strBody);
    if (strId.empty())
    {
        WriteJson(pServerTask, "{\"error\":\"save failed\"}", "500");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"id\":\"" << strId << "\"}";
    WriteJson(pServerTask, oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 获取文本：GET /api/text/<id>
// ----------------------------------------------------------------------------
bool CHttpServerModule::HandleGetText(WFHttpTask* pServerTask, const std::string& strId)
{
    if (s_pStore == nullptr)
    {
        WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strText;
    if (!s_pStore->GetText(strId, strText))
    {
        WriteJson(pServerTask, "{\"error\":\"not found\"}", "404");
        return true;
    }
    WriteText(pServerTask, strText);
    return true;
}

// ----------------------------------------------------------------------------
// 上传文件：POST /api/file —— header X-File-Name 指定文件名，body 为内容
// ----------------------------------------------------------------------------
bool CHttpServerModule::HandleUploadFile(WFHttpTask* pServerTask)
{
    if (s_pStore == nullptr)
    {
        WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strFileName = GetHeader(pServerTask, "X-File-Name");
    // 前端上传时对文件名做 encodeURIComponent 编码（%XX），此处解码还原；
    // 对纯 ASCII 文件名解码是幂等的，直接解码安全。
    strFileName = UrlDecode(strFileName);
    std::string strBody;
    size_t nSize = ReadBody(pServerTask, strBody);
    if (nSize == 0)
    {
        WriteJson(pServerTask, "{\"error\":\"empty body\"}", "400");
        return true;
    }
    std::string strId = s_pStore->SaveFile(strFileName, strBody.data(), strBody.size());
    if (strId.empty())
    {
        WriteJson(pServerTask, "{\"error\":\"save failed\"}", "500");
        return true;
    }
    std::ostringstream oss;
    oss << "{\"id\":\"" << strId << "\"}";
    WriteJson(pServerTask, oss.str());
    return true;
}

// ----------------------------------------------------------------------------
// 下载文件：GET /api/file/<id>
// ----------------------------------------------------------------------------
bool CHttpServerModule::HandleGetFile(WFHttpTask* pServerTask, const std::string& strId)
{
    if (s_pStore == nullptr)
    {
        WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    std::string strName;
    std::vector<char> vecData;
    if (!s_pStore->GetFile(strId, strName, vecData))
    {
        WriteJson(pServerTask, "{\"error\":\"not found\"}", "404");
        return true;
    }
    protocol::HttpResponse* pResp = pServerTask->get_resp();
    pResp->set_status_code("200");
    pResp->add_header_pair("Content-Type", "application/octet-stream");
    // 指定下载文件名（浏览器自动保存）。HTTP 头不允许非 ASCII 字节：
    //   - ASCII 文件名直接用 filename="...";
    //   - 含非 ASCII（如中文）时用 RFC 5987 filename*=UTF-8''<urlencoded>，
    //     并提供 ASCII 回退名（RFC 6266），保证浏览器正确识别中文文件名。
    std::string strDisposition;
    if (HasNonAscii(strName))
    {
        strDisposition = "attachment; filename=\"download.bin\"; filename*=UTF-8''" + UrlEncode(strName);
    }
    else
    {
        strDisposition = "attachment; filename=\"" + strName + "\"";
    }
    pResp->add_header_pair("Content-Disposition", strDisposition.c_str());
    pResp->append_output_body(vecData.data(), vecData.size());
    return true;
}

// ----------------------------------------------------------------------------
// 删除：DELETE /api/item/<id>
// ----------------------------------------------------------------------------
bool CHttpServerModule::HandleDelete(WFHttpTask* pServerTask, const std::string& strId)
{
    if (s_pStore == nullptr)
    {
        WriteJson(pServerTask, "{\"error\":\"store unavailable\"}", "500");
        return true;
    }
    if (!s_pStore->Remove(strId))
    {
        WriteJson(pServerTask, "{\"error\":\"not found\"}", "404");
        return true;
    }
    WriteJson(pServerTask, "{\"ok\":true}");
    return true;
}

// ----------------------------------------------------------------------------
// 响应辅助
// ----------------------------------------------------------------------------
void CHttpServerModule::WriteJson(WFHttpTask* pServerTask, const std::string& strJson, const char* szStatus)
{
    WriteText(pServerTask, strJson, szStatus, "application/json; charset=utf-8");
}

void CHttpServerModule::WriteText(WFHttpTask* pServerTask, const std::string& strBody, const char* szStatus,
                                  const char* szType)
{
    protocol::HttpResponse* pResp = pServerTask->get_resp();
    pResp->set_status_code(szStatus);
    pResp->add_header_pair("Content-Type", szType);
    pResp->append_output_body(strBody.data(), strBody.size());
}

size_t CHttpServerModule::ReadBody(WFHttpTask* pServerTask, std::string& strBody)
{
    protocol::HttpRequest* pReq = pServerTask->get_req();
    const void* pBody = nullptr;
    size_t nLen = 0;
    if (pReq->get_parsed_body(&pBody, &nLen) && nLen > 0)
    {
        strBody.assign(static_cast<const char*>(pBody), nLen);
        return nLen;
    }
    strBody.clear();
    return 0;
}

std::string CHttpServerModule::GetHeader(WFHttpTask* pServerTask, const char* szName)
{
    protocol::HttpRequest* pReq = pServerTask->get_req();
    protocol::HttpHeaderCursor cursor(pReq);
    std::string strName;
    std::string strValue;
    while (cursor.next(strName, strValue))
    {
        // header 名大小写不敏感。
        std::string strLower = strName;
        std::transform(strLower.begin(), strLower.end(), strLower.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        std::string strNeed = szName;
        std::transform(strNeed.begin(), strNeed.end(), strNeed.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        if (strLower == strNeed)
        {
            return strValue;
        }
    }
    return std::string();
}

// ----------------------------------------------------------------------------
// URL 解码
// ----------------------------------------------------------------------------
std::string CHttpServerModule::UrlDecode(const std::string& strEncoded)
{
    std::string strOut;
    strOut.reserve(strEncoded.size());
    for (size_t i = 0; i < strEncoded.size(); ++i)
    {
        if (strEncoded[i] == '%' && i + 2 < strEncoded.size())
        {
            int nHigh = 0, nLow = 0;
            char c1 = strEncoded[i + 1];
            char c2 = strEncoded[i + 2];
            if (c1 >= '0' && c1 <= '9')
                nHigh = c1 - '0';
            else if (c1 >= 'a' && c1 <= 'f')
                nHigh = c1 - 'a' + 10;
            else if (c1 >= 'A' && c1 <= 'F')
                nHigh = c1 - 'A' + 10;
            else
            {
                strOut.push_back(strEncoded[i]);
                continue;
            }
            if (c2 >= '0' && c2 <= '9')
                nLow = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f')
                nLow = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F')
                nLow = c2 - 'A' + 10;
            else
            {
                strOut.push_back(strEncoded[i]);
                continue;
            }
            strOut.push_back(static_cast<char>((nHigh << 4) | nLow));
            i += 2;
        }
        else if (strEncoded[i] == '+')
        {
            strOut.push_back(' ');
        }
        else
        {
            strOut.push_back(strEncoded[i]);
        }
    }
    return strOut;
}

// ----------------------------------------------------------------------------
// URL 编码（RFC 3986 unreserved 保留，其余 %XX）
// ----------------------------------------------------------------------------
std::string CHttpServerModule::UrlEncode(const std::string& strRaw)
{
    static const char* const kHex = "0123456789ABCDEF";
    std::string strOut;
    strOut.reserve(strRaw.size() * 3);
    for (unsigned char c : strRaw)
    {
        // unreserved: A-Z a-z 0-9 - _ . ~
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
        {
            strOut.push_back(static_cast<char>(c));
        }
        else
        {
            strOut.push_back('%');
            strOut.push_back(kHex[(c >> 4) & 0x0F]);
            strOut.push_back(kHex[c & 0x0F]);
        }
    }
    return strOut;
}

// ----------------------------------------------------------------------------
// 判断是否含非 ASCII 字符
// ----------------------------------------------------------------------------
bool CHttpServerModule::HasNonAscii(const std::string& strValue)
{
    for (unsigned char c : strValue)
    {
        if (c >= 0x80)
        {
            return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
// HTML 转义（防 XSS）
// ----------------------------------------------------------------------------
std::string CHttpServerModule::HtmlEscape(const std::string& strRaw)
{
    std::string strOut;
    strOut.reserve(strRaw.size());
    for (char c : strRaw)
    {
        switch (c)
        {
            case '&':
                strOut += "&amp;";
                break;
            case '<':
                strOut += "&lt;";
                break;
            case '>':
                strOut += "&gt;";
                break;
            case '"':
                strOut += "&quot;";
                break;
            case '\'':
                strOut += "&#39;";
                break;
            default:
                strOut.push_back(c);
                break;
        }
    }
    return strOut;
}

SC_BEGIN_INTERFACE_MAP(CHttpServerModule, sc::CModule)
SC_INTERFACE_ENTRY(IHttpService)
SC_END_INTERFACE_MAP(CHttpServerModule, sc::CModule)

}  // namespace datahub
