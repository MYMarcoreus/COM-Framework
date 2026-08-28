#pragma once

#include <ctime>
#include <map>
#include <string>

namespace common {
namespace config {

/// @brief 配置管理器。
///
/// 解析 INI 风格配置（key = value，#/; 注释，[section] 分组）。
/// 键统一格式为 "section.key"（无 section 时为 "key"）。
/// 支持配置文件变更检测与热加载（ReloadIfChanged）。
class CConfig
{
public:
    CConfig();

    // 从文件加载（追加合并，后加载覆盖先加载）。
    bool LoadFile(const std::string& strPath);

    // 解析文本内容。
    bool Parse(const std::string& strContent);

    // 检查配置文件是否变更，变更则重新加载；返回是否发生重载。
    bool ReloadIfChanged();

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
    // 返回文件修改时间；获取失败返回 0。
    static std::time_t FileMtime(const std::string& strPath);

    std::map<std::string, std::string> m_mapValues;
    std::string m_strFilePath;
    std::time_t m_nFileMtime;
};

} // namespace config
} // namespace common
