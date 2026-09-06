#include "Module/Http/CDeviceController.h"

#include <string>

#include "Framework/HttpRouter.h"
#include "Framework/HttpText.h"
#include "Module/Http/CDeviceRegistry.h"

namespace datahub {

namespace {
// 去掉字符串首尾空白。
std::string TrimDeviceId(const std::string& str)
{
    const std::string::size_type nStart = str.find_first_not_of(" \t\r\n");
    if (nStart == std::string::npos)
    {
        return std::string();
    }
    const std::string::size_type nEnd = str.find_last_not_of(" \t\r\n");
    return str.substr(nStart, nEnd - nStart + 1);
}
}  // namespace

/// @brief 创建设备控制器。
CDeviceController::CDeviceController(CDeviceRegistry* pRegistry) : m_pRegistry(pRegistry) {}

/// @brief 注册设备路由。
void CDeviceController::RegisterRoutes(web::CHttpRouter& router)
{
    router.Register({"POST", "/api/device/register",
                     [this](web::CHttpRequest& req, web::CHttpResponse& resp) { return HandleRegister(req, resp); }});
}

/// @brief 注册（幂等）：body=clientId → {token}。
bool CDeviceController::HandleRegister(web::CHttpRequest& req, web::CHttpResponse& resp)
{
    if (m_pRegistry == nullptr)
    {
        resp.WriteJson("{\"error\":\"device registry unavailable\"}", "500");
        return true;
    }
    const std::string strClientId = TrimDeviceId(req.Body());
    if (strClientId.size() < 8 || strClientId.size() > 64)
    {
        resp.WriteJson("{\"error\":\"invalid client id\"}", "400");
        return true;
    }
    const std::string strToken = m_pRegistry->Register(strClientId);
    if (strToken.empty())
    {
        resp.WriteJson("{\"error\":\"register failed\"}", "400");
        return true;
    }
    resp.WriteJson("{\"token\":" + web::CHttpText::JsonString(strToken) + "}");
    return true;
}

}  // namespace datahub
