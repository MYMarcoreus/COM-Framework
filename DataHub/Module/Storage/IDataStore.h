#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"
#include "Module/Tenant/CTenant.h"

namespace sc {

/// @brief 数据项类型。
enum class DataKind : int
{
    kText = 0,  // 文本（粘贴内容）
    kFile = 1,  // 文件（上传的二进制）
};

/// @brief 数据项元信息（列出 / 展示用）。
struct DataItemInfo
{
    std::string strId;       // 数据项标识
    DataKind kind;           // 类型
    std::string strName;     // 文件名（文本时可为空）
    std::string strFrom;     // 来源标识（如 "IP:port"）
    std::uint64_t nSize;     // 字节数
    std::int64_t nCreateMs;  // 创建时间（毫秒）
    std::uint64_t nSeq = 0;  // 单调递增序号（增量同步游标）
};

/// @brief 数据存储接口（按租户隔离）。
///
/// 供 HTTP 服务模块在请求处理时存取数据项；所有操作以租户为隔离边界，
/// 同一数据只在所属租户内可见。数据项标识由实现生成（短码）。
SC_INTERFACE(IDataStore, "datahub::IDataStore", "63821b50-55e9-44df-a88a-8f899df1defb")
{
   public:
    virtual ~IDataStore() {}

    // 以下操作均以租户为隔离边界。

    // 保存文本内容到指定租户，返回生成的数据项标识；失败返回空串。
    virtual std::string SaveText(const CTenant& tenant, const std::string& strContent,
                                 const std::string& strFrom = "") = 0;

    // 保存二进制文件到指定租户，返回生成的数据项标识；失败返回空串。
    // @param strName 文件名（展示用）
    virtual std::string SaveFile(const CTenant& tenant, const std::string& strName, const void* pData,
                                 std::size_t nSize, const std::string& strFrom = "") = 0;

    // 按数据项标识获取租户内元信息；不存在返回 false。
    virtual bool GetInfo(const CTenant& tenant, const std::string& strId, DataItemInfo& info) const = 0;

    // 按数据项标识读取租户内文本内容；成功返回 true。
    virtual bool GetText(const CTenant& tenant, const std::string& strId, std::string& strOut) const = 0;

    // 按数据项标识读取租户内文件内容；成功返回 true。
    virtual bool GetFile(const CTenant& tenant, const std::string& strId, std::string& strName,
                         std::vector<char>& vecData) const = 0;

    // 列出租户内全部数据项（按创建时间倒序）。
    virtual std::vector<DataItemInfo> List(const CTenant& tenant) const = 0;

    // 列出租户内"序号 > nSince"的数据项（按序号升序，旧→新）；nSince=0 返回全部。
    virtual std::vector<DataItemInfo> ListSince(const CTenant& tenant, std::uint64_t nSince) const = 0;

    // 删除租户内数据项；成功返回 true。
    virtual bool Remove(const CTenant& tenant, const std::string& strId) = 0;

    // ---- 服务端管理 / 统计能力（运维控制面 /api/admin 用） ----

    // 租户当前数据项数量。
    virtual std::size_t Count(const CTenant& tenant) const = 0;

    // 租户当前内容占用总字节。
    virtual std::uint64_t TotalBytes(const CTenant& tenant) const = 0;

    // 清空某租户全部数据项（删除租户前调用）。
    virtual void PurgeTenant(const CTenant& tenant) = 0;
};

}  // namespace sc
