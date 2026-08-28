#pragma once

#include <string>

#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"

namespace sc {

/// @brief 配置接口（模块化适配 common::config::CConfig）。
///
/// 使模块通过模块管理器按接口读取配置，而非直接持有 CConfig 实例。
SC_INTERFACE(IConfig, "sc::IConfig", "cfaa634b-1064-40d3-94eb-b8518776bd7e")
{
public:
    virtual ~IConfig() {}

    // 从文件加载配置（追加合并）。
    virtual bool LoadFile(const std::string& strPath) = 0;

    // 读取字符串。
    virtual std::string GetString(const std::string& key, const std::string& def) const = 0;

    // 读取整数。
    virtual int GetInt(const std::string& key, int def) const = 0;

    // 读取布尔值。
    virtual bool GetBool(const std::string& key, bool def) const = 0;

    // 检查配置文件是否变更，变更则重新加载；返回是否发生重载。
    virtual bool ReloadIfChanged() = 0;
};

} // namespace sc
