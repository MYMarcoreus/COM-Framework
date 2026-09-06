#pragma once

#include <string>

#include "Framework/HttpMessage.h"

namespace datahub {

namespace web {
class CHttpRouter;
}

class CDeviceRegistry;

/// @brief 设备控制器 —— 账号凭据注册（修复身份伪造）。
///
/// 设备首次注册获取设备令牌（bearer），此后身份相关 API 携带
/// `X-Client-Id` + `X-Token`。`POST /api/device/register`（body=clientId）
/// 幂等返回令牌；此端点本身免令牌（注册是第一次握手）。
class CDeviceController
{
   public:
    // @param pRegistry 设备注册表（装配层注入）
    explicit CDeviceController(CDeviceRegistry* pRegistry);

    // 注册路由（/api/device/register）。
    void RegisterRoutes(web::CHttpRouter& router);

    // POST /api/device/register：body=clientId → {token}。
    bool HandleRegister(web::CHttpRequest& req, web::CHttpResponse& resp);

   private:
    CDeviceRegistry* m_pRegistry;
};

}  // namespace datahub
