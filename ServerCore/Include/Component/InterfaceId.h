#pragma once

#include <cstdint>
#include <string>

namespace sc {

/// @brief 接口唯一标识（128 位 GUID）。
///
/// 由外部工具（如 uuidgen）生成的固定 GUID 字符串常量解析而来（COM 风格），
/// 每个接口在 IID_XXX() 中定义唯一 GUID + 可读名。
/// 进程内唯一、可比较、可作 map 键；Name() 提供可读名（日志 / 快照）。
class InterfaceId
{
public:
    // 无效标识（全零）。
    InterfaceId() : m_nHigh(0), m_nLow(0), m_strName() {}

    // 从可读名 + 标准 GUID 字符串（8-4-4-4-12）构造；GUID 非法时置全零。
    InterfaceId(const char* strName, const char* strGuid)
        : m_nHigh(0), m_nLow(0), m_strName(strName != nullptr ? strName : "")
    {
        ParseGuid(strGuid, m_nHigh, m_nLow);
    }

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

    // 可读名字（如 "sc::INetwork"）。
    const std::string& Name() const
    {
        return m_strName;
    }

private:
    // 解析标准 GUID 字符串到 128 位；非法时置全零。
    static void ParseGuid(const char* strGuid, uint64_t& nHigh, uint64_t& nLow)
    {
        nHigh = 0;
        nLow = 0;
        if (strGuid == nullptr)
        {
            return;
        }
        unsigned char bytes[16];
        int nByte = 0;
        for (int i = 0; strGuid[i] != '\0' && nByte < 16;)
        {
            if (strGuid[i] == '-')
            {
                ++i;
                continue;
            }
            int nHigh4 = HexValue(strGuid[i]);
            int nLow4 = HexValue(strGuid[i + 1]);
            if (nHigh4 < 0 || nLow4 < 0)
            {
                nHigh = 0;
                nLow = 0;
                return;
            }
            bytes[nByte++] = static_cast<unsigned char>((nHigh4 << 4) | nLow4);
            i += 2;
        }
        if (nByte != 16)
        {
            nHigh = 0;
            nLow = 0;
            return;
        }
        nHigh = 0;
        nLow = 0;
        for (int i = 0; i < 8; ++i)
        {
            nHigh = (nHigh << 8) | bytes[i];
        }
        for (int i = 8; i < 16; ++i)
        {
            nLow = (nLow << 8) | bytes[i];
        }
    }

    // 十六进制字符转数值；非法返回 -1。
    static int HexValue(char c)
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    }

    uint64_t m_nHigh;
    uint64_t m_nLow;
    std::string m_strName;
};

/// @brief 获取 IUnknown 接口标识。
inline const InterfaceId& IID_IUnknown()
{
    static const InterfaceId iid("sc::IUnknown", "0d966519-d922-4be7-baec-bc3c671aa864");
    return iid;
}

} // namespace sc
