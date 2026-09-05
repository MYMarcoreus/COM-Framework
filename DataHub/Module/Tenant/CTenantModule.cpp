#include "Module/Tenant/CTenantModule.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#include "Module/InterfaceMap.h"
#include "Module/ResolveContext.h"

namespace datahub {

// 租户码字符集：去掉易混淆的 0/O、1/I、l（与存储短码一致，码长为 6）。
static const char* const kTenantCodeChars = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz";
static const std::size_t kTenantCodeLen = 6;

namespace {
/// @brief 当前时间（毫秒）。
std::int64_t TenantNowMs()
{
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}
}  // namespace

/// @brief 创建租户模块；构造即内置公共租户（码 "public"）。
CTenantModule::CTenantModule(const CTenantLimits& defaultLimits)
    : sc::CModule("tenant"), m_strDefaultCode("public"), m_defaultLimits(defaultLimits)
{
    CTenant publicTenant;
    publicTenant.strCode = m_strDefaultCode;
    publicTenant.strName = "公共租户";
    publicTenant.limits = m_defaultLimits;
    publicTenant.nCreateMs = TenantNowMs();
    m_mapTenants[m_strDefaultCode] = publicTenant;
}

CTenantModule::~CTenantModule() {}

bool CTenantModule::Initialize(const sc::CResolveContext& ctx)
{
    // 租户注册表无需外部依赖。
    (void)ctx;
    return true;
}

bool CTenantModule::Start()
{
    return true;
}

void CTenantModule::Stop() {}

void CTenantModule::Shutdown() {}

const std::string& CTenantModule::DefaultCode() const
{
    return m_strDefaultCode;
}

/// @brief 生成不与现有租户冲突的短码。
std::string CTenantModule::GenerateCode() const
{
    static std::mt19937 rng(static_cast<unsigned int>(TenantNowMs()));
    static std::uniform_int_distribution<std::size_t> dist(0, strlen(kTenantCodeChars) - 1);
    std::string strCode;
    for (;;)
    {
        strCode.clear();
        for (std::size_t i = 0; i < kTenantCodeLen; ++i)
        {
            strCode.push_back(kTenantCodeChars[dist(rng)]);
        }
        if (strCode != m_strDefaultCode && m_mapTenants.find(strCode) == m_mapTenants.end())
        {
            return strCode;
        }
    }
}

/// @brief 创建租户并生成唯一码。
bool CTenantModule::CreateTenant(const std::string& strName, CTenant& out)
{
    if (strName.size() > 48)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    CTenant tenant;
    tenant.strCode = GenerateCode();
    tenant.strName = strName.empty() ? "未命名租户" : strName;
    tenant.limits = m_defaultLimits;
    tenant.nCreateMs = TenantNowMs();
    m_mapTenants[tenant.strCode] = tenant;
    out = tenant;
    return true;
}

bool CTenantModule::FindTenant(const std::string& strCode, CTenant& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapTenants.find(strCode);
    if (it == m_mapTenants.end())
    {
        return false;
    }
    out = it->second;
    return true;
}

std::size_t CTenantModule::Count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mapTenants.size();
}

SC_BEGIN_INTERFACE_MAP(CTenantModule, sc::CModule)
SC_INTERFACE_ENTRY(ITenantService)
SC_END_INTERFACE_MAP(CTenantModule, sc::CModule)

}  // namespace datahub
