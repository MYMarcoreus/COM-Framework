#include "Config/config.h"

#include <cstdlib>

#include "ini.h"

namespace common {

namespace
{
/// inih 解析回调：将 (section, name, value) 扁平化为 "section.name" 存入 map。
int IniHandler(void* user, const char* section, const char* name, const char* value)
{
    std::map<std::string, std::string>* values =
        static_cast<std::map<std::string, std::string>*>(user);
    std::string key = (section != nullptr && section[0] != '\0')
                          ? (std::string(section) + "." + std::string(name))
                          : std::string(name);
    (*values)[key] = (value != nullptr) ? value : "";
    return 1;
}
} // namespace

/// @brief 创建配置管理器。
CConfig::CConfig()
{
}

/// @brief 从文件加载配置（基于 inih）。
bool CConfig::LoadFile(const std::string& path)
{
    std::map<std::string, std::string> loaded;
    int result = ini_parse(path.c_str(), IniHandler, &loaded);
    if (result != 0)
    {
        return false;
    }
    m_mapValues.insert(loaded.begin(), loaded.end());
    return true;
}

/// @brief 解析文本内容（基于 inih）。
bool CConfig::Parse(const std::string& content)
{
    std::map<std::string, std::string> loaded;
    int result = ini_parse_string(content.c_str(), IniHandler, &loaded);
    if (result != 0)
    {
        return false;
    }
    m_mapValues.insert(loaded.begin(), loaded.end());
    return true;
}

/// @brief 读取字符串配置。
std::string CConfig::GetString(const std::string& key, const std::string& def) const
{
    std::map<std::string, std::string>::const_iterator it = m_mapValues.find(key);
    return (it != m_mapValues.end()) ? it->second : def;
}

/// @brief 读取整数配置。
int CConfig::GetInt(const std::string& key, int def) const
{
    std::map<std::string, std::string>::const_iterator it = m_mapValues.find(key);
    if (it == m_mapValues.end())
    {
        return def;
    }
    return std::atoi(it->second.c_str());
}

/// @brief 读取布尔配置。
bool CConfig::GetBool(const std::string& key, bool def) const
{
    std::map<std::string, std::string>::const_iterator it = m_mapValues.find(key);
    if (it == m_mapValues.end())
    {
        return def;
    }
    std::string value = it->second;
    return (value == "true" || value == "1" || value == "yes" || value == "on");
}

/// @brief 是否包含指定键。
bool CConfig::Has(const std::string& key) const
{
    return m_mapValues.find(key) != m_mapValues.end();
}

/// @brief 清空配置。
void CConfig::Clear()
{
    m_mapValues.clear();
}

} // namespace common
