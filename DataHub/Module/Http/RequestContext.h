#pragma once

#include <string>

#include "Framework/HttpMessage.h"
#include "Module/Tenant/CTenant.h"

namespace datahub {

using sc::CTenant;

/// @brief 请求级上下文（业界惯例：入口装配，业务只读）。
///
/// 生命周期 = 当前请求处理栈（OnRequest 内局部对象）；经 CHttpRequest::UserData()
/// 传递。业务从上下文读取"已解析的当前租户 / 请求标识"等，替代裸 void* 强转与
/// 各自解析头。后续可在此追加鉴权结果、request-id、限流信息（中间件管道的地基）。
struct CRequestContext
{
    CTenant tenant;            // 已解析的当前租户（含码/名称/配额）
    std::string strAccountId;  // 账号（X-Client-Id，缺省对端地址）；授权与成员判定
    std::string strRequestId;  // 请求标识（日志 / 追踪）
    bool bResolved = false;    // 租户是否解析成功（false = 请求带未知租户）

    const CTenant& Tenant() const { return tenant; }
};

/// @brief 取当前请求上下文。
///
/// 入口 OnRequest 把 CRequestContext* 挂到 req.UserData()；控制器据此读取租户。
/// 未挂载时返回空上下文兜底（正常流程不会触发）。
inline const CRequestContext& RequestContextOf(web::CHttpRequest& req)
{
    const CRequestContext* pCtx = static_cast<const CRequestContext*>(req.UserData());
    static const CRequestContext kEmpty;
    return pCtx != nullptr ? *pCtx : kEmpty;
}

}  // namespace datahub
