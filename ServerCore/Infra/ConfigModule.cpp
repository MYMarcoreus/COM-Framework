#include "Infra/ConfigModule.h"

#include "Module/InterfaceMap.h"
#include <string>

namespace sc {

// 接口映射表：暴露本类实现的接口（查表驱动 QueryInterface）。
SC_DEFINE_INTERFACE_MAP(CConfigModule, CModule, IConfig)

/// @brief 创建配置模块。
CConfigModule::CConfigModule() : CModule("config")
{
}

/// @brief 销毁配置模块。
CConfigModule::~CConfigModule()
{
}

/// @brief 初始化模块（配置由外部 LoadFile 加载，此处无动作）。
bool CConfigModule::Initialize(const CResolveContext& /*ctx*/)
{
    return true;
}

/// @brief 模块启动（无独立启动资源）。
bool CConfigModule::Start()
{
    return true;
}

/// @brief 模块停止（配置数据保留）。
void CConfigModule::Stop()
{
}

/// @brief 模块关闭（配置数据由析构释放）。
void CConfigModule::Shutdown()
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

/// @brief 检查配置文件是否变更并热加载。
///
/// @return true 表示发生重载；false 表示未变更或无需重载。
bool CConfigModule::ReloadIfChanged()
{
    return m_config.ReloadIfChanged();
}

} // namespace sc
