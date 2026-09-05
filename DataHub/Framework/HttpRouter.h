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
    const char* prefix;   // 路径前缀（如 "/api/text/"）；exact=true 时作全等匹配
    bool exact;           // true=路径全等；false=前缀匹配
    HttpHandler handler;  // 处理器（可为 lambda / std::bind / 成员函数）
};

/// @brief HTTP 路由注册表。
///
/// 通用 Web 框架组件（无业务依赖）：按"方法 + 路径"注册处理函数并分发。
/// 业务模块在装配期集中注册自己的路由；加新接口不改本类。
/// 前缀命中时，把剩余路径段（如 /api/text/<id> 的 <id>）写入 req.PathParam()。
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
    struct Entry
    {
        HttpRoute route;
    };
    std::vector<Entry> m_vecRoutes;
};

}  // namespace web
}  // namespace datahub
