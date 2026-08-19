#include "Infra/IConfig.h"

#include <string>

namespace sc {

/// @brief 创建配置模块。
CConfigModule::CConfigModule() : CModule("config")
{
}

/// @brief 销毁配置模块。
CConfigModule::~CConfigModule()
{
}

/// @brief 从文件加载配置。
///
/// @return 加载成功返回 true。
bool CConfigModule::LoadFile(const std::string& strPath)
{
    return m_config.LoadFile(strPath);
}

/// @brief 读取字符串。
///
/// @return 对应值；不存在时返回默认值。
std::string CConfigModule::GetString(const std::string& key, const std::string& def) const
{
    return m_config.GetString(key, def);
}

/// @brief 读取整数。
///
/// @return 对应值；不存在或非法时返回默认值。
int CConfigModule::GetInt(const std::string& key, int def) const
{
    return m_config.GetInt(key, def);
}

/// @brief 读取布尔值。
///
/// @return 对应值；不存在时返回默认值。
bool CConfigModule::GetBool(const std::string& key, bool def) const
{
    return m_config.GetBool(key, def);
}

/// @brief 接口查询实现。
bool CConfigModule::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_IConfig()))
    {
        *ppv = static_cast<IConfig*>(this);
        return true;
    }
    return CModule::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
