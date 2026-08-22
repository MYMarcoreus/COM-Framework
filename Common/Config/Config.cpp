#include "Config/Config.h"

#include <cstdlib>

#include <sys/stat.h>

#include "ini.h"

namespace common {

namespace
{
/// inih 解析回调：将 (section, name, value) 扁平化为 "section.name" 存入 map。
int IniHandler(void* pUser, const char* pSection, const char* pName, const char* pValue)
{
    std::map<std::string, std::string>* pValues =
        static_cast<std::map<std::string, std::string>*>(pUser);
    std::string strKey = (pSection != nullptr && pSection[0] != '\0')
                          ? (std::string(pSection) + "." + std::string(pName))
                          : std::string(pName);
    (*pValues)[strKey] = (pValue != nullptr) ? pValue : "";
    return 1;
}
} // namespace

/// @brief 创建配置管理器。
CConfig::CConfig()
{
}

/// @brief 从文件加载配置（基于 inih）。
bool CConfig::LoadFile(const std::string& strPath)
{
    std::map<std::string, std::string> mapLoaded;
    int nResult = ini_parse(strPath.c_str(), IniHandler, &mapLoaded);
    if (nResult != 0)
    {
        return false;
    }
    m_mapValues.insert(mapLoaded.begin(), mapLoaded.end());
    m_strFilePath = strPath;
    m_nFileMtime = FileMtime(strPath);
    return true;
}

/// @brief 检查配置文件是否变更，变更则重新加载。
///
/// 比较文件修改时间；变化时清空并重新加载（整体替换）。
///
/// @return true 表示发生了重载；false 表示未变更或未加载过文件。
bool CConfig::ReloadIfChanged()
{
    if (m_strFilePath.empty())
    {
        return false;
    }
    std::time_t nNewMtime = FileMtime(m_strFilePath);
    if (nNewMtime != m_nFileMtime)
    {
        m_mapValues.clear();
        return LoadFile(m_strFilePath);
    }
    return false;
}

/// @brief 解析文本内容（基于 inih）。
bool CConfig::Parse(const std::string& strContent)
{
    std::map<std::string, std::string> mapLoaded;
    int nResult = ini_parse_string(strContent.c_str(), IniHandler, &mapLoaded);
    if (nResult != 0)
    {
        return false;
    }
    m_mapValues.insert(mapLoaded.begin(), mapLoaded.end());
    return true;
}

/// @brief 读取字符串配置。
std::string CConfig::GetString(const std::string& strKey, const std::string& strDef) const
{
    std::map<std::string, std::string>::const_iterator it = m_mapValues.find(strKey);
    return (it != m_mapValues.end()) ? it->second : strDef;
}

/// @brief 读取整数配置。
int CConfig::GetInt(const std::string& strKey, int nDef) const
{
    std::map<std::string, std::string>::const_iterator it = m_mapValues.find(strKey);
    if (it == m_mapValues.end())
    {
        return nDef;
    }
    return std::atoi(it->second.c_str());
}

/// @brief 读取布尔配置。
bool CConfig::GetBool(const std::string& strKey, bool bDef) const
{
    std::map<std::string, std::string>::const_iterator it = m_mapValues.find(strKey);
    if (it == m_mapValues.end())
    {
        return bDef;
    }
    std::string strValue = it->second;
    return (strValue == "true" || strValue == "1" || strValue == "yes" || strValue == "on");
}

/// @brief 是否包含指定键。
bool CConfig::Has(const std::string& strKey) const
{
    return m_mapValues.find(strKey) != m_mapValues.end();
}

/// @brief 清空配置。
void CConfig::Clear()
{
    m_mapValues.clear();
    m_strFilePath.clear();
    m_nFileMtime = 0;
}

/// @brief 返回文件修改时间。
///
/// @return 修改时间；获取失败返回 0。
std::time_t CConfig::FileMtime(const std::string& strPath)
{
    struct stat st;
    if (::stat(strPath.c_str(), &st) != 0)
    {
        return 0;
    }
    return st.st_mtime;
}

} // namespace common
