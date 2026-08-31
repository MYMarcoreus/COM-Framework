#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Module/InterfaceDecl.h"
#include "Module/IUnknown.h"

namespace sc {

/// @brief 数据项类型。
enum class DataKind : int
{
    kText = 0, // 文本（粘贴内容）
    kFile = 1, // 文件（上传的二进制）
};

/// @brief 数据项元信息（列出 / 展示用）。
struct DataItemInfo
{
    std::string strId;   // 提取码
    DataKind    kind;    // 类型
    std::string strName; // 文件名（文本时可为空）
    std::uint64_t nSize; // 字节数
    std::int64_t  nCreateMs; // 创建时间（毫秒）
};

/// @brief 数据存储接口。
///
/// 供 HTTP 服务模块在请求处理时存取数据项。
/// 提取码由实现生成（短码，便于手机输入）。
SC_INTERFACE(IDataStore, "datahub::IDataStore", "63821b50-55e9-44df-a88a-8f899df1defb")
{
public:
    virtual ~IDataStore() {}

    // 保存文本内容，返回生成的提取码；失败返回空串。
    virtual std::string SaveText(const std::string& strContent) = 0;

    // 保存二进制文件，返回生成的提取码；失败返回空串。
    // @param strName 文件名（展示用）
    // @param pData   数据指针
    // @param nSize   数据字节数
    virtual std::string SaveFile(const std::string& strName,
                                 const void* pData, std::size_t nSize) = 0;

    // 按提取码获取数据项元信息；不存在返回 false。
    virtual bool GetInfo(const std::string& strId, DataItemInfo& info) const = 0;

    // 按提取码读取文本内容；成功返回 true。
    virtual bool GetText(const std::string& strId, std::string& strOut) const = 0;

    // 按提取码读取文件内容；成功返回 true。
    // @param strName 输出文件名
    virtual bool GetFile(const std::string& strId,
                         std::string& strName, std::vector<char>& vecData) const = 0;

    // 列出全部数据项（按创建时间倒序）。
    virtual std::vector<DataItemInfo> List() const = 0;

    // 按提取码删除数据项；成功返回 true。
    virtual bool Remove(const std::string& strId) = 0;
};

} // namespace sc
