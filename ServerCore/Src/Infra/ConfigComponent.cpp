#include "Infra/IConfig.h"

#include <string>

namespace sc {

/// @brief 创建配置组件。
ConfigComponent::ConfigComponent()
{
}

/// @brief 销毁配置组件。
ConfigComponent::~ConfigComponent()
{
}

/// @brief 从文件加载配置。
///
/// @return 加载成功返回 true。
bool ConfigComponent::LoadFile(const std::string& path)
{
    return config_.LoadFile(path);
}

/// @brief 读取字符串。
///
/// @return 对应值；不存在时返回默认值。
std::string ConfigComponent::GetString(const std::string& key, const std::string& def) const
{
    return config_.GetString(key, def);
}

/// @brief 读取整数。
///
/// @return 对应值；不存在或非法时返回默认值。
int ConfigComponent::GetInt(const std::string& key, int def) const
{
    return config_.GetInt(key, def);
}

/// @brief 读取布尔值。
///
/// @return 对应值；不存在时返回默认值。
bool ConfigComponent::GetBool(const std::string& key, bool def) const
{
    return config_.GetBool(key, def);
}

/// @brief 接口查询实现。
bool ConfigComponent::QueryInterfaceImpl(const InterfaceId& iid, void** ppv)
{
    if (std::string(iid) == std::string(IID_IConfig()))
    {
        *ppv = static_cast<IConfig*>(this);
        return true;
    }
    return Component::QueryInterfaceImpl(iid, ppv);
}

} // namespace sc
