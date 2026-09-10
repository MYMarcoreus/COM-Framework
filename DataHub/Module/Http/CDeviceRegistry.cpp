#include "Module/Http/CDeviceRegistry.h"

#include <chrono>
#include <random>
#include <string>

namespace datahub {

namespace {
// 生成随机十六进制令牌（32 字符，128 位熵）。
std::string MakeToken()
{
    static std::mt19937_64 rng(
        static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    static const char* const kHex = "0123456789abcdef";
    std::string strToken;
    strToken.reserve(32);
    for (int i = 0; i < 32; ++i)
    {
        strToken.push_back(kHex[(rng() >> (i % 16)) & 0xF]);
    }
    return strToken;
}
}  // namespace

CDeviceRegistry::CDeviceRegistry() {}

/// @brief 注册（幂等）。
std::string CDeviceRegistry::Register(const std::string& strClientId)
{
    if (strClientId.empty() || strClientId == "unknown:0")
    {
        return std::string();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapToken.find(strClientId);
    if (it != m_mapToken.end())
    {
        return it->second;
    }
    std::string strToken = MakeToken();
    m_mapToken[strClientId] = strToken;
    return strToken;
}

/// @brief 校验令牌归属（常数时间比较，防时序侧信道）。
bool CDeviceRegistry::Verify(const std::string& strClientId, const std::string& strToken) const
{
    if (strClientId.empty() || strToken.empty())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mapToken.find(strClientId);
    if (it == m_mapToken.end())
    {
        return false;
    }
    const std::string& strExpected = it->second;
    if (strExpected.size() != strToken.size())
    {
        return false;
    }
    unsigned char nDiff = 0;
    for (std::size_t i = 0; i < strExpected.size(); ++i)
    {
        nDiff |= static_cast<unsigned char>(strExpected[i] ^ strToken[i]);
    }
    return nDiff == 0;
}

std::size_t CDeviceRegistry::Count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mapToken.size();
}

void CDeviceRegistry::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mapToken.clear();
}

}  // namespace datahub
