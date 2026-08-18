#include "Config/Config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace common {

/// @brief 创建配置管理器。
Config::Config()
{
}

/// @brief 从文件加载配置。
bool Config::LoadFile(const std::string& path)
{
    std::ifstream in(path.c_str());
    if (!in.is_open())
    {
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return Parse(buffer.str());
}

/// @brief 解析文本内容。
///
/// 支持空行、#/; 注释、[section] 分组与 key = value 键值对。
bool Config::Parse(const std::string& content)
{
    std::string section;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line))
    {
        // ① 去空白，跳过空行与注释
        std::string s = Trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';')
        {
            continue;
        }
        // ② 解析 [section]
        if (s[0] == '[' && s[s.size() - 1] == ']')
        {
            section = Trim(s.substr(1, s.size() - 2));
            continue;
        }
        // ③ 解析 key = value
        size_t eq = s.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        std::string key = Trim(s.substr(0, eq));
        std::string value = Trim(s.substr(eq + 1));
        if (key.empty())
        {
            continue;
        }
        // 去掉行内注释
        size_t comment = value.find_first_of("#;");
        if (comment != std::string::npos)
        {
            value = Trim(value.substr(0, comment));
        }
        if (!section.empty())
        {
            key = section + "." + key;
        }
        values_[key] = value;
    }
    return true;
}

/// @brief 读取字符串配置。
std::string Config::GetString(const std::string& key, const std::string& def) const
{
    std::map<std::string, std::string>::const_iterator it = values_.find(key);
    return (it != values_.end()) ? it->second : def;
}

/// @brief 读取整数配置。
int Config::GetInt(const std::string& key, int def) const
{
    std::map<std::string, std::string>::const_iterator it = values_.find(key);
    if (it == values_.end())
    {
        return def;
    }
    return std::atoi(it->second.c_str());
}

/// @brief 读取布尔配置。
bool Config::GetBool(const std::string& key, bool def) const
{
    std::map<std::string, std::string>::const_iterator it = values_.find(key);
    if (it == values_.end())
    {
        return def;
    }
    std::string value = it->second;
    return (value == "true" || value == "1" || value == "yes" || value == "on");
}

/// @brief 是否包含指定键。
bool Config::Has(const std::string& key) const
{
    return values_.find(key) != values_.end();
}

/// @brief 清空配置。
void Config::Clear()
{
    values_.clear();
}

/// @brief 去除首尾空白。
std::string Config::Trim(const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
    {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

} // namespace common
