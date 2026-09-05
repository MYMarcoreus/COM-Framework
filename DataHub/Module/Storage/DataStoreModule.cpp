#include "Module/Storage/DataStoreModule.h"

#include <utility>

#include "Module/InterfaceMap.h"
#include "Module/ResolveContext.h"

namespace datahub {

/// @brief 创建数据存储模块（按租户隔离）。
CDataStoreModule::CDataStoreModule() : sc::CModule("store") {}

/// @brief 销毁数据存储模块。
CDataStoreModule::~CDataStoreModule() {}

bool CDataStoreModule::Initialize(const sc::CResolveContext& ctx)
{
    // 存储组件无需依赖外部接口，无初始化动作。
    (void)ctx;
    return true;
}

bool CDataStoreModule::Start()
{
    return true;
}

void CDataStoreModule::Stop() {}

void CDataStoreModule::Shutdown()
{
    m_mapStores.clear();
}

/// @brief 取（或惰性创建）某租户的数据存储实例，并应用其容量限制。
common::storage::CFileStore* CDataStoreModule::EnsureStore(const CTenant& tenant) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapStores.find(tenant.strCode);
    if (it == m_mapStores.end())
    {
        std::unique_ptr<common::storage::CFileStore> pStore(new common::storage::CFileStore());
        pStore->SetMaxItems(tenant.limits.nMaxItems);
        pStore->SetMaxTotalBytes(tenant.limits.nMaxTotalBytes);
        pStore->SetMaxItemBytes(tenant.limits.nMaxItemBytes);
        it = m_mapStores.insert(std::make_pair(tenant.strCode, std::move(pStore))).first;
    }
    return it->second.get();
}

std::string CDataStoreModule::SaveText(const CTenant& tenant, const std::string& strContent,
                                       const std::string& strFrom)
{
    return EnsureStore(tenant)->SaveText(strContent, strFrom);
}

std::string CDataStoreModule::SaveFile(const CTenant& tenant, const std::string& strName, const void* pData,
                                       std::size_t nSize, const std::string& strFrom)
{
    return EnsureStore(tenant)->SaveFile(strName, pData, nSize, strFrom);
}

bool CDataStoreModule::GetInfo(const CTenant& tenant, const std::string& strId, DataItemInfo& info) const
{
    common::storage::StoreItemInfo storeInfo;
    if (!EnsureStore(tenant)->GetInfo(strId, storeInfo))
    {
        return false;
    }
    info.strId = storeInfo.strId;
    info.kind = storeInfo.kind == common::storage::StoreItemKind::kText ? DataKind::kText : DataKind::kFile;
    info.strName = storeInfo.strName;
    info.strFrom = storeInfo.strFrom;
    info.nSize = storeInfo.nSize;
    info.nCreateMs = storeInfo.nCreateMs;
    info.nSeq = storeInfo.nSeq;
    return true;
}

bool CDataStoreModule::GetText(const CTenant& tenant, const std::string& strId, std::string& strOut) const
{
    return EnsureStore(tenant)->GetText(strId, strOut);
}

bool CDataStoreModule::GetFile(const CTenant& tenant, const std::string& strId, std::string& strName,
                               std::vector<char>& vecData) const
{
    return EnsureStore(tenant)->GetFile(strId, strName, vecData);
}

std::vector<DataItemInfo> CDataStoreModule::List(const CTenant& tenant) const
{
    std::vector<DataItemInfo> vecResult;
    std::vector<common::storage::StoreItemInfo> vecStore = EnsureStore(tenant)->List();
    vecResult.reserve(vecStore.size());
    for (const common::storage::StoreItemInfo& storeInfo : vecStore)
    {
        DataItemInfo info;
        info.strId = storeInfo.strId;
        info.kind = storeInfo.kind == common::storage::StoreItemKind::kText ? DataKind::kText : DataKind::kFile;
        info.strName = storeInfo.strName;
        info.strFrom = storeInfo.strFrom;
        info.nSize = storeInfo.nSize;
        info.nCreateMs = storeInfo.nCreateMs;
        info.nSeq = storeInfo.nSeq;
        vecResult.push_back(info);
    }
    return vecResult;
}

std::vector<DataItemInfo> CDataStoreModule::ListSince(const CTenant& tenant, std::uint64_t nSince) const
{
    std::vector<DataItemInfo> vecResult;
    std::vector<common::storage::StoreItemInfo> vecStore = EnsureStore(tenant)->ListSince(nSince);
    vecResult.reserve(vecStore.size());
    for (const common::storage::StoreItemInfo& storeInfo : vecStore)
    {
        DataItemInfo info;
        info.strId = storeInfo.strId;
        info.kind = storeInfo.kind == common::storage::StoreItemKind::kText ? DataKind::kText : DataKind::kFile;
        info.strName = storeInfo.strName;
        info.strFrom = storeInfo.strFrom;
        info.nSize = storeInfo.nSize;
        info.nCreateMs = storeInfo.nCreateMs;
        info.nSeq = storeInfo.nSeq;
        vecResult.push_back(info);
    }
    return vecResult;
}

bool CDataStoreModule::Remove(const CTenant& tenant, const std::string& strId)
{
    return EnsureStore(tenant)->Remove(strId);
}

SC_BEGIN_INTERFACE_MAP(CDataStoreModule, sc::CModule)
SC_INTERFACE_ENTRY(IDataStore)
SC_END_INTERFACE_MAP(CDataStoreModule, sc::CModule)

}  // namespace datahub
