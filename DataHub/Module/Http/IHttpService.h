#pragma once

#include <cstdint>
#include <string>

#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"

namespace sc {

/// @brief HTTP 数据传输服务接口（多租户：租户 = 隔离单元）。
///
/// 基于 Sogou Workflow（WFHttpServer）实现的 HTTP 服务：
///   - 业务请求用 X-Tenant 头标明当前租户（缺省为公共租户 "public"）；
///     消息 / 文件 / 成员 / 配额均以租户为边界隔离，凭 6 位租户码加入共享。
///   - GET  /                —— 返回内置网页（含租户切换器）
///   - POST /api/tenant      —— 创建租户（body 为名称），返回 {code,name}
///   - GET  /api/tenant/info —— 按 ?code= 查询租户（供凭码加入前校验）
///   - POST /api/text        —— 上传文本到当前租户；返回 {"id":"XXXXXX"}
///   - GET  /api/text/<id>   —— 取当前租户内文本
///   - POST /api/file        —— 上传文件；header X-File-Name 指定文件名
///   - GET  /api/file/<id>   —— 下载当前租户内文件（含 Range 分段）
///   - GET  /api/list        —— 列当前租户数据（支持 ?since= 增量）
///   - DELETE /api/item/<id> —— 删除当前租户内数据（归属校验）
SC_INTERFACE(IHttpService, "datahub::IHttpService", "21d79b83-abe3-4d5f-9363-ba306305ce9d")
{
   public:
    virtual ~IHttpService() {}

    // 当前监听端口。
    virtual std::uint16_t Port() const = 0;

    // 状态描述（供 GetStatus 汇总）。
    virtual std::string Status() const = 0;
};

}  // namespace sc
