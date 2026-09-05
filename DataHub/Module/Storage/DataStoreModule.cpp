#include "Module/Storage/DataStoreModule.h"

#include "Module/InterfaceMap.h"
#include "Module/ResolveContext.h"

namespace datahub {

/// @brief 创建数据存储模块（共享表多租户：单实例存储）。
CDataStoreModule::CDataStoreModule() : sc::CModule("store"), m_pStore(new common::storage::CFileStore()) {}

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
    m_pStore->Clear();
}

/// @brief 租户实体的容量限制 → 共享存储的 StoreLimits。
common::storage::StoreLimits CDataStoreModule::LimitsOf(const CTenant& tenant)
{
    common::storage::StoreLimits limits;
    limits.nMaxItems = tenant.limits.nMaxItems;
    limits.nMaxTotalBytes = tenant.limits.nMaxTotalBytes;
    limits.nMaxItemBytes = tenant.limits.nMaxItemBytes;
    return limits;
}

std::string CDataStoreModule::SaveText(const CTenant& tenant, const std::string& strContent, const std::string& strFrom)
{
    return m_pStore->SaveText(tenant.strCode, strContent, strFrom, LimitsOf(tenant));
}

std::string CDataStoreModule::SaveFile(const CTenant& tenant, const std::string& strName, const void* pData,
                                       std::size_t nSize, const std::string& strFrom)
{
    return m_pStore->SaveFile(tenant.strCode, strName, pData, nSize, strFrom, LimitsOf(tenant));
}

bool CDataStoreModule::GetInfo(const CTenant& tenant, const std::string& strId, DataItemInfo& info) const
{
    common::storage::StoreItemInfo storeInfo;
    if (!m_pStore->GetInfo(tenant.strCode, strId, storeInfo))
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
    return m_pStore->GetText(tenant.strCode, strId, strOut);
}

bool CDataStoreModule::GetFile(const CTenant& tenant, const std::string& strId, std::string& strName,
                               std::vector<char>& vecData) const
{
    return m_pStore->GetFile(tenant.strCode, strId, strName, vecData);
}

std::vector<DataItemInfo> CDataStoreModule::List(const CTenant& tenant) const
{
    std::vector<DataItemInfo> vecResult;
    std::vector<common::storage::StoreItemInfo> vecStore = m_pStore->List(tenant.strCode);
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
    std::vector<common::storage::StoreItemInfo> vecStore = m_pStore->ListSince(tenant.strCode, nSince);
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
    return m_pStore->Remove(tenant.strCode, strId);
}

SC_BEGIN_INTERFACE_MAP(CDataStoreModule, sc::CModule)
SC_INTERFACE_ENTRY(IDataStore)
SC_END_INTERFACE_MAP(CDataStoreModule, sc::CModule)

}  // namespace datahub
