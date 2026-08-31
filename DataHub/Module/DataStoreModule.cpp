#include "Module/DataStoreModule.h"

#include "Module/InterfaceMap.h"
#include "Module/ResolveContext.h"

namespace datahub {

/// @brief 创建数据存储模块。
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

void CDataStoreModule::Shutdown() {}

std::string CDataStoreModule::SaveText(const std::string& strContent)
{
    return m_pStore->SaveText(strContent);
}

std::string CDataStoreModule::SaveFile(const std::string& strName, const void* pData, std::size_t nSize)
{
    return m_pStore->SaveFile(strName, pData, nSize);
}

bool CDataStoreModule::GetInfo(const std::string& strId, DataItemInfo& info) const
{
    common::storage::StoreItemInfo storeInfo;
    if (!m_pStore->GetInfo(strId, storeInfo))
    {
        return false;
    }
    info.strId = storeInfo.strId;
    info.kind = storeInfo.kind == common::storage::StoreItemKind::kText ? DataKind::kText : DataKind::kFile;
    info.strName = storeInfo.strName;
    info.nSize = storeInfo.nSize;
    info.nCreateMs = storeInfo.nCreateMs;
    return true;
}

bool CDataStoreModule::GetText(const std::string& strId, std::string& strOut) const
{
    return m_pStore->GetText(strId, strOut);
}

bool CDataStoreModule::GetFile(const std::string& strId, std::string& strName, std::vector<char>& vecData) const
{
    return m_pStore->GetFile(strId, strName, vecData);
}

std::vector<DataItemInfo> CDataStoreModule::List() const
{
    std::vector<DataItemInfo> vecResult;
    std::vector<common::storage::StoreItemInfo> vecStore = m_pStore->List();
    vecResult.reserve(vecStore.size());
    for (const common::storage::StoreItemInfo& storeInfo : vecStore)
    {
        DataItemInfo info;
        info.strId = storeInfo.strId;
        info.kind = storeInfo.kind == common::storage::StoreItemKind::kText ? DataKind::kText : DataKind::kFile;
        info.strName = storeInfo.strName;
        info.nSize = storeInfo.nSize;
        info.nCreateMs = storeInfo.nCreateMs;
        vecResult.push_back(info);
    }
    return vecResult;
}

bool CDataStoreModule::Remove(const std::string& strId)
{
    return m_pStore->Remove(strId);
}

SC_BEGIN_INTERFACE_MAP(CDataStoreModule, sc::CModule)
SC_INTERFACE_ENTRY(IDataStore)
SC_END_INTERFACE_MAP(CDataStoreModule, sc::CModule)

}  // namespace datahub
