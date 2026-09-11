#include "Storage/CFileStore.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>

namespace common {
namespace storage {

// 短码字符集：去掉易混淆的 0/O、1/I、l。
static const char* const kIdChars = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz";

/// @brief 创建存储组件（默认短码长度 6）。
CFileStore::CFileStore() : m_nIdLen(6)
{}

/// @brief 创建存储组件（自定义短码长度）。
CFileStore::CFileStore(std::size_t nIdLen) : m_nIdLen(nIdLen > 0 ? nIdLen : 6)
{}

/// @brief 生成随机字符。
char CFileStore::RandomChar() const
{
    static std::mt19937 rng(static_cast<unsigned int>(std::chrono::steady_clock::now().time_since_epoch().count()));
    static std::uniform_int_distribution<size_t> dist(0, strlen(kIdChars) - 1);
    return kIdChars[dist(rng)];
}

/// @brief 生成不重复的短码（跨租户全局唯一，等同主键）。
std::string CFileStore::GenerateId() const
{
    std::string strId;
    for (;;)
    {
        strId.clear();
        for (std::size_t i = 0; i < m_nIdLen; ++i)
        {
            strId.push_back(RandomChar());
        }
        if (m_mapItems.find(strId) == m_mapItems.end())
        {
            return strId;
        }
    }
}

/// @brief 当前时间（毫秒）。
static std::int64_t NowMs()
{
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string CFileStore::SaveText(const std::string& strTenant, const std::string& strContent,
                                 const std::string& strFrom, const StoreLimits& limits)
{
    if (strTenant.empty() || strContent.empty())
    {
        return std::string();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t nPayload = static_cast<std::uint64_t>(strContent.size());
    // 按租户配额检查（等价对租户计数）。先取该租户当前统计。
    TenantStat stat;
    auto itStat = m_mapStats.find(strTenant);
    if (itStat != m_mapStats.end())
    {
        stat = itStat->second;
    }
    if ((limits.nMaxItems > 0 && stat.nItems >= limits.nMaxItems) ||
        (limits.nMaxItemBytes > 0 && nPayload > limits.nMaxItemBytes) ||
        (limits.nMaxTotalBytes > 0 && stat.nBytes + nPayload > limits.nMaxTotalBytes))
    {
        return std::string();
    }
    Item item;
    item.kind = StoreItemKind::kText;
    item.strTenant = strTenant;
    item.strFrom = strFrom;
    item.strText = strContent;
    item.nCreateMs = NowMs();
    item.nSeq = ++m_nNextSeq;
    std::string strId = GenerateId();
    m_mapItems[strId] = std::move(item);
    stat.nItems += 1;
    stat.nBytes += nPayload;
    m_mapStats[strTenant] = stat;
    return strId;
}

std::string CFileStore::SaveFile(const std::string& strTenant, const std::string& strName, const void* pData,
                                 std::size_t nSize, const std::string& strFrom, const StoreLimits& limits)
{
    if (strTenant.empty() || pData == nullptr || nSize == 0)
    {
        return std::string();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t nPayload = static_cast<std::uint64_t>(nSize);
    TenantStat stat;
    auto itStat = m_mapStats.find(strTenant);
    if (itStat != m_mapStats.end())
    {
        stat = itStat->second;
    }
    if ((limits.nMaxItems > 0 && stat.nItems >= limits.nMaxItems) ||
        (limits.nMaxItemBytes > 0 && nPayload > limits.nMaxItemBytes) ||
        (limits.nMaxTotalBytes > 0 && stat.nBytes + nPayload > limits.nMaxTotalBytes))
    {
        return std::string();
    }
    Item item;
    item.kind = StoreItemKind::kFile;
    item.strTenant = strTenant;
    item.strName = strName.empty() ? "file.bin" : strName;
    item.strFrom = strFrom;
    const char* pBytes = static_cast<const char*>(pData);
    item.vecData.assign(pBytes, pBytes + nSize);
    item.nCreateMs = NowMs();
    item.nSeq = ++m_nNextSeq;
    std::string strId = GenerateId();
    m_mapItems[strId] = std::move(item);
    stat.nItems += 1;
    stat.nBytes += nPayload;
    m_mapStats[strTenant] = stat;
    return strId;
}

/// @brief 数据项内容字节（文本或文件）。
bool CFileStore::GetInfo(const std::string& strTenant, const std::string& strId, StoreItemInfo& info) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapItems.find(strId);
    if (it == m_mapItems.end() || it->second.strTenant != strTenant)
    {
        return false;
    }
    const Item& item = it->second;
    info.strId = strId;
    info.kind = item.kind;
    info.strTenant = item.strTenant;
    info.strName = item.strName;
    info.strFrom = item.strFrom;
    info.nSize = item.kind == StoreItemKind::kText ? static_cast<std::uint64_t>(item.strText.size())
                                                   : static_cast<std::uint64_t>(item.vecData.size());
    info.nCreateMs = item.nCreateMs;
    info.nSeq = item.nSeq;
    return true;
}

bool CFileStore::GetText(const std::string& strTenant, const std::string& strId, std::string& strOut) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapItems.find(strId);
    if (it == m_mapItems.end() || it->second.strTenant != strTenant || it->second.kind != StoreItemKind::kText)
    {
        return false;
    }
    strOut = it->second.strText;
    return true;
}

bool CFileStore::GetFile(const std::string& strTenant, const std::string& strId, std::string& strName,
                         std::vector<char>& vecData) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapItems.find(strId);
    if (it == m_mapItems.end() || it->second.strTenant != strTenant || it->second.kind != StoreItemKind::kFile)
    {
        return false;
    }
    strName = it->second.strName;
    vecData = it->second.vecData;
    return true;
}

std::vector<StoreItemInfo> CFileStore::List(const std::string& strTenant) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<StoreItemInfo> vecResult;
    for (const auto& pair : m_mapItems)
    {
        const Item& item = pair.second;
        if (item.strTenant != strTenant)
        {
            continue;  // 等价 WHERE tenant_id = ?
        }
        StoreItemInfo info;
        info.strId = pair.first;
        info.kind = item.kind;
        info.strTenant = item.strTenant;
        info.strName = item.strName;
        info.strFrom = item.strFrom;
        info.nSize = item.kind == StoreItemKind::kText ? static_cast<std::uint64_t>(item.strText.size())
                                                       : static_cast<std::uint64_t>(item.vecData.size());
        info.nCreateMs = item.nCreateMs;
        info.nSeq = item.nSeq;
        vecResult.push_back(info);
    }
    // 按创建时间倒序（新的在前）。
    std::sort(vecResult.begin(), vecResult.end(),
              [](const StoreItemInfo& a, const StoreItemInfo& b)
              {
                  return a.nCreateMs > b.nCreateMs;
              });
    return vecResult;
}

std::vector<StoreItemInfo> CFileStore::ListSince(const std::string& strTenant, std::uint64_t nSince) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<StoreItemInfo> vecResult;
    for (const auto& pair : m_mapItems)
    {
        const Item& item = pair.second;
        if (item.strTenant != strTenant || item.nSeq <= nSince)
        {
            continue;
        }
        StoreItemInfo info;
        info.strId = pair.first;
        info.kind = item.kind;
        info.strTenant = item.strTenant;
        info.strName = item.strName;
        info.strFrom = item.strFrom;
        info.nSize = item.kind == StoreItemKind::kText ? static_cast<std::uint64_t>(item.strText.size())
                                                       : static_cast<std::uint64_t>(item.vecData.size());
        info.nCreateMs = item.nCreateMs;
        info.nSeq = item.nSeq;
        vecResult.push_back(info);
    }
    // 按序号升序（旧→新），便于客户端按顺序追加渲染。
    std::sort(vecResult.begin(), vecResult.end(),
              [](const StoreItemInfo& a, const StoreItemInfo& b)
              {
                  return a.nSeq < b.nSeq;
              });
    return vecResult;
}

bool CFileStore::Remove(const std::string& strTenant, const std::string& strId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapItems.find(strId);
    if (it == m_mapItems.end() || it->second.strTenant != strTenant)
    {
        return false;  // 跨租户删除视为不存在
    }
    const Item& item = it->second;
    const std::uint64_t nPayload = item.kind == StoreItemKind::kText ? static_cast<std::uint64_t>(item.strText.size())
                                                                     : static_cast<std::uint64_t>(item.vecData.size());
    m_mapItems.erase(it);
    // 更新该租户统计；归零则移除。
    auto itStat = m_mapStats.find(strTenant);
    if (itStat != m_mapStats.end())
    {
        TenantStat stat = itStat->second;
        stat.nItems = stat.nItems > 0 ? stat.nItems - 1 : 0;
        stat.nBytes = stat.nBytes >= nPayload ? stat.nBytes - nPayload : 0;
        if (stat.nItems == 0 && stat.nBytes == 0)
        {
            m_mapStats.erase(itStat);
        }
        else
        {
            itStat->second = stat;
        }
    }
    return true;
}

std::size_t CFileStore::Count(const std::string& strTenant) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapStats.find(strTenant);
    return it == m_mapStats.end() ? 0 : it->second.nItems;
}

std::uint64_t CFileStore::TotalBytes(const std::string& strTenant) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapStats.find(strTenant);
    return it == m_mapStats.end() ? 0 : it->second.nBytes;
}

void CFileStore::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mapItems.clear();
    m_mapStats.clear();
    // 注意：m_nNextSeq 不重置，避免清空后新数据序号回退导致游标错乱。
}

}  // namespace storage
}  // namespace common
