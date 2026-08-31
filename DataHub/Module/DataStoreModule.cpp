#include "Module/DataStoreModule.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>

#include "Module/InterfaceMap.h"
#include "Module/ResolveContext.h"

namespace datahub {

// 提取码字符集：去掉易混淆的 0/O、1/I、l。
static const char* const kIdChars =
    "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz";
static const size_t kIdLen = 6;

/// @brief 创建数据存储模块。
CDataStoreModule::CDataStoreModule()
    : sc::CModule("store")
{
}

/// @brief 销毁数据存储模块。
CDataStoreModule::~CDataStoreModule()
{
}

bool CDataStoreModule::Initialize(const sc::CResolveContext& ctx)
{
    // 内存存储无需依赖外部接口，无初始化动作。
    (void)ctx;
    return true;
}

bool CDataStoreModule::Start()
{
    return true;
}

void CDataStoreModule::Stop()
{
}

void CDataStoreModule::Shutdown()
{
}

/// @brief 生成随机字符。
char CDataStoreModule::RandomChar() const
{
    static std::mt19937 rng(static_cast<unsigned int>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    static std::uniform_int_distribution<size_t> dist(0, strlen(kIdChars) - 1);
    return kIdChars[dist(rng)];
}

/// @brief 生成不重复的提取码。
///
/// 生成 6 位随机码；若与已有项冲突则重新生成。
std::string CDataStoreModule::GenerateId() const
{
    std::string strId;
    for (;;)
    {
        strId.clear();
        for (size_t i = 0; i < kIdLen; ++i)
        {
            strId.push_back(RandomChar());
        }
        if (m_mapItems.find(strId) == m_mapItems.end())
        {
            return strId;
        }
    }
}

std::string CDataStoreModule::SaveText(const std::string& strContent)
{
    if (strContent.empty())
    {
        return std::string();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    Item item;
    item.kind = DataKind::kText;
    item.strText = strContent;
    item.nCreateMs = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    std::string strId = GenerateId();
    m_mapItems[strId] = std::move(item);
    return strId;
}

std::string CDataStoreModule::SaveFile(const std::string& strName,
                                       const void* pData, std::size_t nSize)
{
    if (pData == nullptr || nSize == 0)
    {
        return std::string();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    Item item;
    item.kind = DataKind::kFile;
    item.strName = strName.empty() ? "file.bin" : strName;
    const char* pBytes = static_cast<const char*>(pData);
    item.vecData.assign(pBytes, pBytes + nSize);
    item.nCreateMs = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    std::string strId = GenerateId();
    m_mapItems[strId] = std::move(item);
    return strId;
}

bool CDataStoreModule::GetInfo(const std::string& strId, DataItemInfo& info) const
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
    info.nSize = item.kind == DataKind::kText
                     ? static_cast<std::uint64_t>(item.strText.size())
                     : static_cast<std::uint64_t>(item.vecData.size());
    info.nCreateMs = item.nCreateMs;
    return true;
}

bool CDataStoreModule::GetText(const std::string& strId, std::string& strOut) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapItems.find(strId);
    if (it == m_mapItems.end() || it->second.kind != DataKind::kText)
    {
        return false;
    }
    strOut = it->second.strText;
    return true;
}

bool CDataStoreModule::GetFile(const std::string& strId,
                               std::string& strName, std::vector<char>& vecData) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapItems.find(strId);
    if (it == m_mapItems.end() || it->second.kind != DataKind::kFile)
    {
        return false;
    }
    strName = it->second.strName;
    vecData = it->second.vecData;
    return true;
}

std::vector<DataItemInfo> CDataStoreModule::List() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<DataItemInfo> vecResult;
    vecResult.reserve(m_mapItems.size());
    for (const auto& pair : m_mapItems)
    {
        DataItemInfo info;
        const Item& item = pair.second;
        info.strId = pair.first;
        info.kind = item.kind;
        info.strName = item.strName;
        info.nSize = item.kind == DataKind::kText
                         ? static_cast<std::uint64_t>(item.strText.size())
                         : static_cast<std::uint64_t>(item.vecData.size());
        info.nCreateMs = item.nCreateMs;
        vecResult.push_back(info);
    }
    // 按创建时间倒序（新的在前）。
    std::sort(vecResult.begin(), vecResult.end(),
              [](const DataItemInfo& a, const DataItemInfo& b)
              { return a.nCreateMs > b.nCreateMs; });
    return vecResult;
}

bool CDataStoreModule::Remove(const std::string& strId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mapItems.erase(strId) > 0;
}

SC_BEGIN_INTERFACE_MAP(CDataStoreModule, sc::CModule)
    SC_INTERFACE_ENTRY(IDataStore)
SC_END_INTERFACE_MAP(CDataStoreModule, sc::CModule)

} // namespace datahub
