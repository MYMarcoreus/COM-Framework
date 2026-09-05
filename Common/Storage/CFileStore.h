#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace common {
namespace storage {

/// @brief 数据项类型。
enum class StoreItemKind : int
{
    kText = 0,  // 文本
    kFile = 1,  // 文件（二进制）
};

/// @brief 租户容量限制（0 = 不限制）。保存时传入，约束对应租户。
struct StoreLimits
{
    std::size_t nMaxItems = 0;         // 该租户最大条数
    std::uint64_t nMaxTotalBytes = 0;  // 该租户内容总字节上限
    std::uint64_t nMaxItemBytes = 0;   // 该租户单条字节上限
};

/// @brief 数据项元信息（列出 / 展示用）。
struct StoreItemInfo
{
    std::string strId;       // 数据项标识（短码，全局唯一）
    StoreItemKind kind;      // 类型
    std::string strTenant;   // 所属租户（tenant_id，共享表隔离维度）
    std::string strName;     // 文件名（文本时可为空）
    std::string strFrom;     // 来源标识（如 "IP:port"；可为空）
    std::uint64_t nSize;     // 字节数
    std::int64_t nCreateMs;  // 创建时间（毫秒）
    std::uint64_t nSeq;      // 全局单调递增序号（保存时分配，增量同步游标）
};

/// @brief 共享表多租户文件存储（纯内存、线程安全）。
///
/// 采用业界主流的多租户数据模型：单张"表"（m_mapItems）+ 每行 strTenant
/// （tenant_id）列；短码全局唯一；所有读取/列举/删除都强制按租户过滤
/// （等价于 SQL 的 WHERE tenant_id=?），业务不可能跨租户读到数据。
/// - 线程安全：所有操作内部加锁，可跨线程调用；
/// - 全局序号 nSeq 单调递增（相当于自增主键）：各租户的增量游标因此天然单调；
/// - 配额按租户：保存时传入 StoreLimits，按租户独立计数/限额；
/// - 纯内存：进程退出即清空（不落盘）。
///
/// 用法：
/// @code
///   common::storage::CFileStore store;
///   std::string id = store.SaveText("t1", "hello");   // 保存到租户 t1
///   std::string text;
///   if (store.GetText("t1", id, text)) { ... }        // 须同租户才可读
///   auto items = store.ListSince("t1", 0);            // 仅 t1 的数据
///   store.Remove("t1", id);
/// @endcode
class CFileStore
{
   public:
    CFileStore();

    // 设置短码长度（默认 6；构造函数调用）。
    explicit CFileStore(std::size_t nIdLen);

    // 保存文本到指定租户，返回全局唯一短码；失败（含配额超限）返回空串。
    std::string SaveText(const std::string& strTenant, const std::string& strContent, const std::string& strFrom = "",
                         const StoreLimits& limits = StoreLimits());

    // 保存二进制文件到指定租户，返回全局唯一短码；失败返回空串。
    // @param strName 文件名（展示用；为空时自动填 "file.bin"）
    std::string SaveFile(const std::string& strTenant, const std::string& strName, const void* pData, std::size_t nSize,
                         const std::string& strFrom = "", const StoreLimits& limits = StoreLimits());

    // 按短码获取指定租户内数据项元信息；不存在 / 不属于该租户返回 false。
    bool GetInfo(const std::string& strTenant, const std::string& strId, StoreItemInfo& info) const;

    // 读取指定租户内文本内容；成功返回 true。
    bool GetText(const std::string& strTenant, const std::string& strId, std::string& strOut) const;

    // 读取指定租户内文件内容；成功返回 true。
    bool GetFile(const std::string& strTenant, const std::string& strId, std::string& strName,
                 std::vector<char>& vecData) const;

    // 列出指定租户全部数据项（按创建时间倒序）。
    std::vector<StoreItemInfo> List(const std::string& strTenant) const;

    // 列出指定租户内"序号 > nSince"的数据项（按序号升序，旧→新）；nSince=0 返回全部。
    // 供前端游标增量同步，避免每次全量拉取。
    std::vector<StoreItemInfo> ListSince(const std::string& strTenant, std::uint64_t nSince) const;

    // 删除指定租户内数据项（跨租户视为不存在）；成功返回 true。
    bool Remove(const std::string& strTenant, const std::string& strId);

    // 指定租户当前数据项数量。
    std::size_t Count(const std::string& strTenant) const;

    // 指定租户当前内容占用总字节。
    std::uint64_t TotalBytes(const std::string& strTenant) const;

    // 清空全部租户的数据。
    void Clear();

   private:
    // 数据项（内部表示）。
    struct Item
    {
        StoreItemKind kind;
        std::string strTenant;  // 所属租户（tenant_id）
        std::string strName;
        std::string strFrom;        // 来源标识（如 "IP:port"）
        std::string strText;        // 文本内容
        std::vector<char> vecData;  // 文件内容
        std::int64_t nCreateMs;
        std::uint64_t nSeq;  // 全局序号（保存时分配）
    };

    // 租户统计（配额计数用）。
    struct TenantStat
    {
        std::size_t nItems = 0;
        std::uint64_t nBytes = 0;
    };

    // 生成不重复的短码。
    std::string GenerateId() const;

    // 从短码字符集中取随机字符。
    char RandomChar() const;

    std::size_t m_nIdLen;
    mutable std::mutex m_mutex;
    std::map<std::string, Item> m_mapItems;        // 全局"表"：id → item
    std::map<std::string, TenantStat> m_mapStats;  // 租户统计（配额计数）
    std::uint64_t m_nNextSeq = 0;                  // 全局序号（只增不减，勿随 Clear 重置）
};

}  // namespace storage
}  // namespace common
