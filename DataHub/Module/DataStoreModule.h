#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Module/IDataStore.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"

namespace datahub {

using sc::DataKind;
using sc::DataItemInfo;
using sc::IDataStore;

/// @brief 数据存储模块。
///
/// 内存存储数据项（文本 / 文件），线程安全；提取码为 6 位大小写字母数字。
/// 模块名 "store"。不落盘（精简版：数据仅存内存，进程退出即清空；
/// 如需持久化可扩展）。
class CDataStoreModule : public sc::CModule, public IDataStore
{
public:
    CDataStoreModule();

    virtual ~CDataStoreModule();

    bool Initialize(const sc::CResolveContext& ctx) override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    std::string SaveText(const std::string& strContent) override;
    std::string SaveFile(const std::string& strName,
                         const void* pData, std::size_t nSize) override;
    bool GetInfo(const std::string& strId, DataItemInfo& info) const override;
    bool GetText(const std::string& strId, std::string& strOut) const override;
    bool GetFile(const std::string& strId,
                 std::string& strName, std::vector<char>& vecData) const override;
    std::vector<DataItemInfo> List() const override;
    bool Remove(const std::string& strId) override;

    SC_DECLARE_INTERFACE_MAP();

private:
    // 数据项（内部表示）。
    struct Item
    {
        DataKind    kind;
        std::string strName;
        std::string strText;      // 文本内容
        std::vector<char> vecData; // 文件内容
        std::int64_t nCreateMs;
    };

    // 生成不重复的提取码。
    std::string GenerateId() const;

    // 从 ID 字符集中取随机字符。
    char RandomChar() const;

    mutable std::mutex m_mutex;
    std::map<std::string, Item> m_mapItems;
};

} // namespace datahub
