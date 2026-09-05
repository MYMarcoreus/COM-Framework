#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Framework/HttpMessage.h"

namespace datahub {
namespace web {

// 路由处理函数：接收请求/响应封装，返回是否已写响应。
typedef std::function<bool(CHttpRequest&, CHttpResponse&)> HttpHandler;

/// @brief HTTP 路由项。
struct HttpRoute
{
    const char* method;   // "GET"/"POST"/"DELETE"...
    const char* pattern;  // 路径模板，如 "/api/list" 精确、"/api/text/{id}" 捕获段
    HttpHandler handler;  // 处理器（可为 lambda / std::bind / 成员函数）
};

/// @brief HTTP 路由注册表。
///
/// 通用 Web 框架组件（无业务依赖）：按"方法 + 路径模板"注册处理函数并分发。
/// 路径模板：字面段精确匹配；"{name}" 段捕获任意单段，分发时写入
/// req.PathParam()（多个捕获段以 "/" 连接，单参数场景即取首段）。
/// 业务在各自 Controller::RegisterRoutes 中注册；加新接口不改本类。
/// 线程安全：注册与分发可跨线程调用（分发为只读查表）。
class CHttpRouter
{
   public:
    CHttpRouter();

    // 注册路由项。返回 true 成功。
    bool Register(const HttpRoute& route);

    // 按方法 + 路径匹配并调用；命中返回其 handler 结果（true=已写响应）。
    // 未命中返回 false（调用方可回 404 或尝试其他兜底）。
    bool Dispatch(CHttpRequest& req, CHttpResponse& resp);

    // 当前已注册路由数。
    std::size_t Count() const;

   private:
    // 把路径模板拆成段；"{name}" 标记为捕获段。
    void ParsePattern(const std::string& strPattern, std::vector<std::string>& vecSegs,
                      std::vector<bool>& vecIsCapture) const;

    struct Entry
    {
        HttpRoute route;
        std::vector<std::string> vecSegs;   // 模板段（已解析）
        std::vector<bool> vecIsCapture;     // 每段是否捕获
        std::vector<std::string> vecParams; // 捕获参数名（调试/预留）
    };
    std::vector<Entry> m_vecRoutes;
};

}  // namespace web
}  // namespace datahub
