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
CFileStore::CFileStore() : m_nIdLen(6) {}

/// @brief 创建存储组件（自定义短码长度）。
CFileStore::CFileStore(std::size_t nIdLen) : m_nIdLen(nIdLen > 0 ? nIdLen : 6) {}

/// @brief 生成随机字符。
char CFileStore::RandomChar() const
{
    static std::mt19937 rng(static_cast<unsigned int>(std::chrono::steady_clock::now().time_since_epoch().count()));
    static std::uniform_int_distribution<size_t> dist(0, strlen(kIdChars) - 1);
    return kIdChars[dist(rng)];
}

/// @brief 生成不重复的短码。
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

std::string CFileStore::SaveText(const std::string& strContent, const std::string& strFrom)
{
    if (strContent.empty())
    {
        return std::string();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    Item item;
    item.kind = StoreItemKind::kText;
    item.strFrom = strFrom;
    item.strText = strContent;
    item.nCreateMs = NowMs();
    std::string strId = GenerateId();
    m_mapItems[strId] = std::move(item);
    return strId;
}

std::string CFileStore::SaveFile(const std::string& strName, const void* pData, std::size_t nSize,
                                 const std::string& strFrom)
{
    if (pData == nullptr || nSize == 0)
    {
        return std::string();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    Item item;
    item.kind = StoreItemKind::kFile;
    item.strName = strName.empty() ? "file.bin" : strName;
    item.strFrom = strFrom;
    const char* pBytes = static_cast<const char*>(pData);
    item.vecData.assign(pBytes, pBytes + nSize);
    item.nCreateMs = NowMs();
    std::string strId = GenerateId();
    m_mapItems[strId] = std::move(item);
    return strId;
}

bool CFileStore::GetInfo(const std::string& strId, StoreItemInfo& info) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapItems.find(strId);
    if (it == m_mapItems.end())
    {
        return false;
    }
    const Item& item = it->second;
    info.strId = strId;
    info.kind = item.kind;
    info.strName = item.strName;
    info.strFrom = item.strFrom;
    info.nSize = item.kind == StoreItemKind::kText ? static_cast<std::uint64_t>(item.strText.size())
                                                   : static_cast<std::uint64_t>(item.vecData.size());
    info.nCreateMs = item.nCreateMs;
    return true;
}

bool CFileStore::GetText(const std::string& strId, std::string& strOut) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapItems.find(strId);
    if (it == m_mapItems.end() || it->second.kind != StoreItemKind::kText)
    {
        return false;
    }
    strOut = it->second.strText;
    return true;
}

bool CFileStore::GetFile(const std::string& strId, std::string& strName, std::vector<char>& vecData) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapItems.find(strId);
    if (it == m_mapItems.end() || it->second.kind != StoreItemKind::kFile)
    {
        return false;
    }
    strName = it->second.strName;
    vecData = it->second.vecData;
    return true;
}

std::vector<StoreItemInfo> CFileStore::List() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<StoreItemInfo> vecResult;
    vecResult.reserve(m_mapItems.size());
    for (const auto& pair : m_mapItems)
    {
        StoreItemInfo info;
        const Item& item = pair.second;
        info.strId = pair.first;
        info.kind = item.kind;
        info.strName = item.strName;
        info.strFrom = item.strFrom;
        info.nSize = item.kind == StoreItemKind::kText ? static_cast<std::uint64_t>(item.strText.size())
                                                       : static_cast<std::uint64_t>(item.vecData.size());
        info.nCreateMs = item.nCreateMs;
        vecResult.push_back(info);
    }
    // 按创建时间倒序（新的在前）。
    std::sort(vecResult.begin(), vecResult.end(),
              [](const StoreItemInfo& a, const StoreItemInfo& b) { return a.nCreateMs > b.nCreateMs; });
    return vecResult;
}

bool CFileStore::Remove(const std::string& strId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mapItems.erase(strId) > 0;
}

std::size_t CFileStore::Size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mapItems.size();
}

void CFileStore::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mapItems.clear();
}

}  // namespace storage
}  // namespace common
