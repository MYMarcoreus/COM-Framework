#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "Module/Storage/IDataStore.h"
#include "Module/Tenant/CTenant.h"
#include "Storage/CFileStore.h"

namespace datahub {

using sc::CTenant;
using sc::DataItemInfo;
using sc::DataKind;
using sc::IDataStore;

/// @brief 数据存储模块（按租户隔离）。
///
/// 为每个租户（空间）维护独立的 common::storage::CFileStore 实例：
/// 同一数据只在所属租户内可见；容量限制取自租户实体，每租户独立计数。
/// 纯内存、线程安全；不落盘（如需持久化可替换/扩展后端）。
/// 模块名 "store"。
class CDataStoreModule : public sc::CModule, public IDataStore
{
   public:
    CDataStoreModule();

    virtual ~CDataStoreModule();

    bool Initialize(const sc::CResolveContext& ctx) override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    std::string SaveText(const CTenant& tenant, const std::string& strContent,
                         const std::string& strFrom = "") override;
    std::string SaveFile(const CTenant& tenant, const std::string& strName, const void* pData, std::size_t nSize,
                         const std::string& strFrom = "") override;
    bool GetInfo(const CTenant& tenant, const std::string& strId, DataItemInfo& info) const override;
    bool GetText(const CTenant& tenant, const std::string& strId, std::string& strOut) const override;
    bool GetFile(const CTenant& tenant, const std::string& strId, std::string& strName,
                 std::vector<char>& vecData) const override;
    std::vector<DataItemInfo> List(const CTenant& tenant) const override;
    std::vector<DataItemInfo> ListSince(const CTenant& tenant, std::uint64_t nSince) const override;
    bool Remove(const CTenant& tenant, const std::string& strId) override;

    SC_DECLARE_INTERFACE_MAP();

   private:
    // 取（或惰性创建）某租户的数据存储实例并应用其容量限制。
    common::storage::CFileStore* EnsureStore(const CTenant& tenant) const;

    mutable std::mutex m_mutex;
    mutable std::map<std::string, std::unique_ptr<common::storage::CFileStore> > m_mapStores;
};

}  // namespace datahub
