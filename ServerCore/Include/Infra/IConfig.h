#pragma once

#include <string>

#include "Component/Component.h"
#include "Config/Config.h"

namespace sc {

/// @brief 获取 IConfig 接口标识。
inline const InterfaceId& IID_IConfig()
{
    static const InterfaceId iid = "sc::IConfig";
    return iid;
}

/// @brief 配置接口（组件化适配 common::CConfig）。
///
/// 使模块通过组件管理器按接口读取配置，而非直接持有 CConfig 实例。
class IConfig : public virtual IUnknown
{
public:
    virtual ~IConfig() {}

    // 从文件加载配置（追加合并）。
    virtual bool LoadFile(const std::string& path) = 0;

    // 读取字符串。
    virtual std::string GetString(const std::string& key, const std::string& def) const = 0;

    // 读取整数。
    virtual int GetInt(const std::string& key, int def) const = 0;

    // 读取布尔值。
    virtual bool GetBool(const std::string& key, bool def) const = 0;
};

/// @brief 配置组件。
///
/// 内部持有独立 common::CConfig 实例。
class CConfigComponent : public CComponent, public IConfig
{
public:
    CConfigComponent();

    virtual ~CConfigComponent();

    bool LoadFile(const std::string& path) override;
    std::string GetString(const std::string& key, const std::string& def) const override;
    int GetInt(const std::string& key, int def) const override;
    bool GetBool(const std::string& key, bool def) const override;

protected:
    // 接口查询实现。
    bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv) override;

private:
    common::CConfig m_config;
};

} // namespace sc
