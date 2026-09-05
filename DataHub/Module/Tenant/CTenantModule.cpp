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

/// @brief 创建租户并生成唯一码；创建者（非空账号）成为 Owner。
bool CTenantModule::CreateTenant(const std::string& strName, const std::string& strOwnerAccount, CTenant& out)
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
    // 创建者记为 Owner。
    if (!strOwnerAccount.empty())
    {
        sc::CTenantMember member;
        member.strAccountId = strOwnerAccount;
        member.role = sc::TenantRole::kOwner;
        member.nJoinMs = tenant.nCreateMs;
        m_mapMembers[tenant.strCode][strOwnerAccount] = member;
    }
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

/// @brief 账号凭码加入租户；未在花名册则作为 Member 加入。
bool CTenantModule::JoinTenant(const std::string& strCode, const std::string& strAccount, sc::TenantRole& out)
{
    if (strAccount.empty())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_mapTenants.find(strCode) == m_mapTenants.end())
    {
        return false;  // 租户不存在
    }
    if (strCode == m_strDefaultCode)
    {
        out = sc::TenantRole::kMember;  // 公共租户：人人皆成员
        return true;
    }
    auto itAccount = m_mapMembers[strCode].find(strAccount);
    if (itAccount != m_mapMembers[strCode].end())
    {
        out = itAccount->second.role;
        return true;
    }
    sc::CTenantMember member;
    member.strAccountId = strAccount;
    member.role = sc::TenantRole::kMember;
    member.nJoinMs = TenantNowMs();
    m_mapMembers[strCode][strAccount] = member;
    out = sc::TenantRole::kMember;
    return true;
}

/// @brief 查询账号在某租户的角色；公共租户恒为 Member；未加入返回 false。
bool CTenantModule::TenantRoleOf(const std::string& strCode, const std::string& strAccount, sc::TenantRole& out) const
{
    if (strCode == m_strDefaultCode)
    {
        out = sc::TenantRole::kMember;
        return true;
    }
    if (strAccount.empty())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto itTenant = m_mapMembers.find(strCode);
    if (itTenant == m_mapMembers.end())
    {
        return false;
    }
    auto itAccount = itTenant->second.find(strAccount);
    if (itAccount == itTenant->second.end())
    {
        return false;
    }
    out = itAccount->second.role;
    return true;
}

/// @brief 某租户的成员花名册（公共租户为空）。
bool CTenantModule::ListMembers(const std::string& strCode, std::vector<sc::CTenantMember>& vecOut) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    vecOut.clear();
    if (strCode == m_strDefaultCode)
    {
        return true;
    }
    auto itTenant = m_mapMembers.find(strCode);
    if (itTenant == m_mapMembers.end())
    {
        return true;
    }
    vecOut.reserve(itTenant->second.size());
    for (const auto& pair : itTenant->second)
    {
        vecOut.push_back(pair.second);
    }
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
