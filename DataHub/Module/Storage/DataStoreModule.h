#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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

/// @brief 数据存储模块（共享表多租户，业界主流模型）。
///
/// 内部只持有一个 common::storage::CFileStore（共享"表"），每行带租户码
/// （tenant_id）；本模块把所有接口的 CTenant 翻译为"租户码 + 该租户配额"
/// 传给存储，存储层强制按租户过滤。等同"共享表 + WHERE tenant_id=?"。
/// 纯内存、线程安全；不落盘（如需持久化可替换后端实现）。模块名 "store"。
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
    // 把租户实体换算为共享存储的容量限制（0 = 不限制）。
    static common::storage::StoreLimits LimitsOf(const CTenant& tenant);

    std::unique_ptr<common::storage::CFileStore> m_pStore;  // 共享表存储（单实例）
};

}  // namespace datahub
