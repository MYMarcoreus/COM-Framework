#include "Framework/HttpRouter.h"

#include <strings.h>

namespace datahub {
namespace web {

CHttpRouter::CHttpRouter() {}

/// @brief 把路径拆成段（按 '/' 分割，忽略首尾空段）。
static void SplitPath(const std::string& strPath, std::vector<std::string>& vecSegs)
{
    vecSegs.clear();
    std::string strSeg;
    for (char c : strPath)
    {
        if (c == '/')
        {
            if (!strSeg.empty())
            {
                vecSegs.push_back(strSeg);
                strSeg.clear();
            }
        }
        else
        {
            strSeg.push_back(c);
        }
    }
    if (!strSeg.empty())
    {
        vecSegs.push_back(strSeg);
    }
}

/// @brief 判断某段是否为捕获段（{name} 形式）。
static bool IsCaptureSeg(const std::string& strSeg)
{
    return strSeg.size() >= 3 && strSeg.front() == '{' && strSeg.back() == '}';
}

bool CHttpRouter::Register(const HttpRoute& route)
{
    if (route.method == nullptr || route.pattern == nullptr || !route.handler)
    {
        return false;
    }
    Entry entry;
    entry.route = route;
    // 预解析模板段。
    std::vector<std::string> vecSegs;
    SplitPath(route.pattern, vecSegs);
    for (const std::string& strSeg : vecSegs)
    {
        entry.vecSegs.push_back(strSeg);
        entry.vecIsCapture.push_back(IsCaptureSeg(strSeg));
    }
    m_vecRoutes.push_back(entry);
    return true;
}

bool CHttpRouter::Dispatch(CHttpRequest& req, CHttpResponse& resp)
{
    const std::string strMethod = req.Method();
    std::vector<std::string> vecPathSegs;
    SplitPath(req.Path(), vecPathSegs);

    for (const Entry& entry : m_vecRoutes)
    {
        const HttpRoute& route = entry.route;
        // 方法匹配（大小写不敏感）。
        if (::strcasecmp(route.method, strMethod.c_str()) != 0)
        {
            continue;
        }
        // 段数不一致：不匹配。
        if (entry.vecSegs.size() != vecPathSegs.size())
        {
            continue;
        }
        // 逐段匹配：字面段须相等，捕获段收集值。
        std::string strParams;
        bool bMatched = true;
        for (std::size_t i = 0; i < entry.vecSegs.size(); ++i)
        {
            if (entry.vecIsCapture[i])
            {
                if (!strParams.empty())
                {
                    strParams += "/";
                }
                strParams += vecPathSegs[i];
            }
            else if (entry.vecSegs[i] != vecPathSegs[i])
            {
                bMatched = false;
                break;
            }
        }
        if (!bMatched)
        {
            continue;
        }
        // 命中：把捕获段写入 PathParam。
        req.SetPathParam(strParams);
        return route.handler(req, resp);
    }
    return false;
}

std::size_t CHttpRouter::Count() const
{
    return m_vecRoutes.size();
}

}  // namespace web
}  // namespace datahub
