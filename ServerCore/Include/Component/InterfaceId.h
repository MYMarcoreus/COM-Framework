#pragma once

#include <cstdint>
#include <string>

namespace sc {

namespace detail
{
/// FNV-1a 64 位哈希（用于从接口名字符串派生 GUID）。
inline uint64_t HashFnv1a(const char* strData, size_t nLen, uint64_t nSeed)
{
    uint64_t nHash = nSeed;
    for (size_t i = 0; i < nLen; ++i)
    {
        nHash ^= static_cast<unsigned char>(strData[i]);
        nHash *= 1099511628211ULL;
    }
    return nHash;
}
} // namespace detail

/// @brief 接口唯一标识（128 位 GUID）。
///
/// 由接口名字符串经稳定哈希派生（FNV-1a 128 位扩展），进程内唯一且保留可读名。
/// 与 COM 的 IID 对应：比较 / 索引使用 128 位 GUID，Name() 提供可读名（日志 / 快照）。
class InterfaceId
{
public:
    // 无效标识（全零）。
    InterfaceId() : m_nHigh(0), m_nLow(0), m_strName() {}

    // 从接口名字符串构造（哈希生成 GUID）。
    InterfaceId(const char* strName)
    {
        if (strName == nullptr)
        {
            m_nHigh = 0;
            m_nLow = 0;
            m_strName.clear();
            return;
        }
        m_strName = strName;
        m_nHigh = detail::HashFnv1a(m_strName.data(), m_strName.size(),
                                    1469598103934665603ULL);
        m_nLow = detail::HashFnv1a(m_strName.data(), m_strName.size(),
                                   1469598103934665603ULL ^ 0x9E3779B97F4A7C15ULL);
    }

    InterfaceId(const std::string& strName) : InterfaceId(strName.c_str()) {}

    // 是否有效（非全零）。
    bool IsValid() const
    {
        return m_nHigh != 0 || m_nLow != 0;
    }

    bool operator==(const InterfaceId& other) const
    {
        return m_nHigh == other.m_nHigh && m_nLow == other.m_nLow;
    }

    bool operator!=(const InterfaceId& other) const
    {
        return !(*this == other);
    }

    bool operator<(const InterfaceId& other) const
    {
        if (m_nHigh != other.m_nHigh)
        {
            return m_nHigh < other.m_nHigh;
        }
        return m_nLow < other.m_nLow;
    }

    // 可读名字（构造时传入的接口名字符串）。
    const std::string& Name() const
    {
        return m_strName;
    }

private:
    uint64_t m_nHigh;
    uint64_t m_nLow;
    std::string m_strName;
};

/// @brief 获取 IUnknown 接口标识。
inline const InterfaceId& IID_IUnknown()
{
    static const InterfaceId iid = "sc::IUnknown";
    return iid;
}

} // namespace sc
