#pragma once

#include <cstdint>
#include <string>

#include "Module/InterfaceDecl.h"
#include "Module/IUnknown.h"

namespace sc {

/// @brief HTTP 数据传输服务接口。
///
/// 基于 Sogou Workflow（WFHttpServer）实现的 HTTP 服务：
///   - GET  /                —— 返回内置网页（手机 / 电脑浏览器直接使用）
///   - POST /api/text        —— 上传文本，body 为内容；返回 {"id":"XXXXXX"}
///   - GET  /api/text/<id>   —— 按提取码获取文本
///   - POST /api/file        —— 上传文件；header X-File-Name 指定文件名；
///                              body 为文件内容；返回 {"id":"XXXXXX"}
///   - GET  /api/file/<id>   —— 按提取码下载文件（响应头含文件名）
///   - GET  /api/list        —— 列出全部数据项（JSON）
///   - DELETE /api/item/<id> —— 删除数据项
SC_INTERFACE(IHttpService, "datahub::IHttpService", "21d79b83-abe3-4d5f-9363-ba306305ce9d")
{
public:
    virtual ~IHttpService() {}

    // 当前监听端口。
    virtual std::uint16_t Port() const = 0;

    // 状态描述（供 GetStatus 汇总）。
    virtual std::string Status() const = 0;
};

} // namespace sc
