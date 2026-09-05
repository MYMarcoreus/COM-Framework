#include "Framework/HttpRouter.h"

#include <strings.h>

namespace datahub {
namespace web {

CHttpRouter::CHttpRouter() {}

bool CHttpRouter::Register(const HttpRoute& route)
{
    if (route.method == nullptr || route.prefix == nullptr || !route.handler)
    {
        return false;
    }
    Entry entry;
    entry.route = route;
    m_vecRoutes.push_back(entry);
    return true;
}

bool CHttpRouter::Dispatch(WFHttpTask* pServerTask, const std::string& strMethod, const std::string& strPath)
{
    for (const Entry& entry : m_vecRoutes)
    {
        const HttpRoute& route = entry.route;
        // 方法匹配（大小写不敏感）。
        if (::strcasecmp(route.method, strMethod.c_str()) != 0)
        {
            continue;
        }
        // 路径匹配：exact 全等 or 前缀。
        const std::string strPrefix = route.prefix;
        bool bMatch = route.exact ? (strPath == strPrefix) : (strPath.compare(0, strPrefix.size(), strPrefix) == 0);
        if (!bMatch)
        {
            continue;
        }
        // 命中：调用 handler（传入完整路径，供前缀路由提取剩余段）。
        return route.handler(pServerTask, strPath);
    }
    return false;
}

std::size_t CHttpRouter::Count() const
{
    return m_vecRoutes.size();
}

}  // namespace web
}  // namespace datahub
