#pragma once

#include <string>

#include "Config/Config.h"
#include "Infra/IConfig.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"

namespace sc {

/// @brief 配置模块。
///
/// 内部持有独立 common::CConfig 实例。
class CConfigModule : public CModule, public IConfig
{
public:
    CConfigModule();

    virtual ~CConfigModule();

    // 生命周期：配置由外部 LoadFile 加载，无独立生命周期资源。
    bool Initialize(const CResolveContext& ctx) override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    bool LoadFile(const std::string& path) override;
    std::string GetString(const std::string& key, const std::string& def) const override;
    int GetInt(const std::string& key, int def) const override;
    bool GetBool(const std::string& key, bool def) const override;
    bool ReloadIfChanged() override;

    SC_DECLARE_INTERFACE_MAP();

private:
    common::CConfig m_config;
};

} // namespace sc
