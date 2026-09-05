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

/// @brief 数据项元信息（列出 / 展示用）。
struct StoreItemInfo
{
    std::string strId;       // 数据项标识（短码）
    StoreItemKind kind;      // 类型
    std::string strName;     // 文件名（文本时可为空）
    std::string strFrom;     // 来源标识（如 "IP:port"；可为空）
    std::uint64_t nSize;     // 字节数
    std::int64_t nCreateMs;  // 创建时间（毫秒）
    std::uint64_t nSeq;      // 单调递增序号（保存时分配，增量同步游标）
};

/// @brief 通用文件存储组件（纯内存、线程安全）。
///
/// 以短码为键存储数据项（文本 / 二进制文件），供任意服务器项目复用。
/// - 线程安全：所有操作内部加锁，可跨线程调用；
/// - 短码生成：默认 6 位随机字符集（去掉易混淆的 0/O、1/I、l），可自定义长度；
/// - 纯内存：数据存内存，进程退出即清空（不落盘）。
///
/// 用法：
/// @code
///   common::storage::CFileStore store;
///   std::string id = store.SaveText("hello");          // 保存文本
///   std::string fid = store.SaveFile("a.txt", data, n); // 保存文件
///   std::string text;
///   if (store.GetText(id, text)) { ... }               // 读取文本
///   std::vector<char> bytes;
///   std::string name;
///   if (store.GetFile(fid, name, bytes)) { ... }       // 读取文件
/// @endcode
class CFileStore
{
   public:
    CFileStore();

    // 设置短码长度（默认 6；构造函数调用）。
    explicit CFileStore(std::size_t nIdLen);

    // 保存文本内容，返回生成的短码；失败返回空串。
    // @param strFrom 来源标识（如 "IP:port"；可选，默认空）。
    std::string SaveText(const std::string& strContent, const std::string& strFrom = "");

    // 保存二进制文件，返回生成的短码；失败返回空串。
    // @param strName 文件名（展示用；为空时自动填 "file.bin"）
    // @param pData   数据指针
    // @param nSize   数据字节数
    // @param strFrom 来源标识（如 "IP:port"；可选，默认空）。
    std::string SaveFile(const std::string& strName, const void* pData, std::size_t nSize,
                         const std::string& strFrom = "");

    // 按短码获取数据项元信息；不存在返回 false。
    bool GetInfo(const std::string& strId, StoreItemInfo& info) const;

    // 按短码读取文本内容；成功返回 true。
    bool GetText(const std::string& strId, std::string& strOut) const;

    // 按短码读取文件内容；成功返回 true。
    // @param strName 输出文件名
    // @param vecData 输出文件内容
    bool GetFile(const std::string& strId, std::string& strName, std::vector<char>& vecData) const;

    // 列出全部数据项（按创建时间倒序）。
    std::vector<StoreItemInfo> List() const;

    // 列出"序号 > nSince"的数据项（按序号升序，旧→新）；nSince=0 返回全部。
    // 供前端游标增量同步，避免每次全量拉取。
    std::vector<StoreItemInfo> ListSince(std::uint64_t nSince) const;

    // 按短码删除数据项；成功返回 true。
    bool Remove(const std::string& strId);

    // 容量/数量上限配置（默认 0 = 不限制）。保存超出上限时返回空串失败。
    void SetMaxItems(std::size_t nMax);
    void SetMaxTotalBytes(std::uint64_t nMax);
    void SetMaxItemBytes(std::uint64_t nMax);

    // 当前已占用内容总字节数（文本/文件内容合计）。
    std::uint64_t TotalBytes() const;

    // 当前数据项数量。
    std::size_t Size() const;

    // 清空全部数据项。
    void Clear();

   private:
    // 数据项（内部表示）。
    struct Item
    {
        StoreItemKind kind;
        std::string strName;
        std::string strFrom;        // 来源标识（如 "IP:port"）
        std::string strText;        // 文本内容
        std::vector<char> vecData;  // 文件内容
        std::int64_t nCreateMs;
        std::uint64_t nSeq;  // 单调递增序号（保存时分配）
    };

    // 生成不重复的短码。
    std::string GenerateId() const;

    // 从短码字符集中取随机字符。
    char RandomChar() const;

    std::size_t m_nIdLen;
    mutable std::mutex m_mutex;
    std::map<std::string, Item> m_mapItems;

    // 序号与配额（在锁内读写）。
    std::uint64_t m_nNextSeq = 0;        // 下一可用序号（只增不减，勿随 Clear 重置）
    std::size_t m_nMaxItems = 0;         // 0 = 不限制条数
    std::uint64_t m_nMaxTotalBytes = 0;  // 0 = 不限制总字节
    std::uint64_t m_nMaxItemBytes = 0;   // 0 = 不限制单条字节
    std::uint64_t m_nTotalBytes = 0;     // 当前占用总字节
};

}  // namespace storage
}  // namespace common
