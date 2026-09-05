#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Module/Storage/IDataStore.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "Storage/CFileStore.h"

namespace datahub {

using sc::DataItemInfo;
using sc::DataKind;
using sc::IDataStore;

/// @brief 数据存储模块。
///
/// 委托通用文件存储组件 common::storage::CFileStore 实现
/// （纯内存、线程安全、短码生成），本模块仅做接口适配。
/// 模块名 "store"。不落盘（数据仅存内存，进程退出即清空；
/// 如需持久化可扩展 CFileStore）。
class CDataStoreModule : public sc::CModule, public IDataStore
{
   public:
    // @param nMaxItems      最大数据条数（0 = 不限制）
    // @param nMaxTotalBytes 最大内容总字节（0 = 不限制）
    // @param nMaxItemBytes  单条内容最大字节（0 = 不限制）
    CDataStoreModule(std::size_t nMaxItems = 0, std::uint64_t nMaxTotalBytes = 0, std::uint64_t nMaxItemBytes = 0);

    virtual ~CDataStoreModule();

    bool Initialize(const sc::CResolveContext& ctx) override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    std::string SaveText(const std::string& strContent, const std::string& strFrom = "") override;
    std::string SaveFile(const std::string& strName, const void* pData, std::size_t nSize,
                         const std::string& strFrom = "") override;
    bool GetInfo(const std::string& strId, DataItemInfo& info) const override;
    bool GetText(const std::string& strId, std::string& strOut) const override;
    bool GetFile(const std::string& strId, std::string& strName, std::vector<char>& vecData) const override;
    std::vector<DataItemInfo> List() const override;
    std::vector<DataItemInfo> ListSince(std::uint64_t nSince) const override;
    bool Remove(const std::string& strId) override;

    SC_DECLARE_INTERFACE_MAP();

   private:
    // 通用文件存储组件（纯内存、线程安全、短码生成）。
    std::unique_ptr<common::storage::CFileStore> m_pStore;
};

}  // namespace datahub
