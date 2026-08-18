#pragma once

#include <map>
#include <string>

namespace common {

/// @brief 配置管理器。
///
/// 解析 INI 风格配置（key = value，#/; 注释，[section] 分组）。
/// 键统一格式为 "section.key"（无 section 时为 "key"）。
class CConfig
{
public:
    CConfig();

    // 从文件加载（追加合并，后加载覆盖先加载）。
    bool LoadFile(const std::string& strPath);

    // 解析文本内容。
    bool Parse(const std::string& strContent);

    // 读取字符串。
    std::string GetString(const std::string& strKey, const std::string& strDef = "") const;

    // 读取整数。
    int GetInt(const std::string& strKey, int nDef = 0) const;

    // 读取布尔值。
    bool GetBool(const std::string& strKey, bool bDef = false) const;

    // 是否包含键。
    bool Has(const std::string& strKey) const;

    // 清空配置。
    void Clear();

private:
    std::map<std::string, std::string> m_mapValues;
};

} // namespace common
