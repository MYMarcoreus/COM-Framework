#include "Serialization/BinaryReader.h"

#include <cstring>

namespace common {
namespace serialization {

/// @brief 从缓冲创建读取器。
CBinaryReader::CBinaryReader(const char* pData, size_t nSize)
    : m_pData(pData != nullptr ? pData : ""), m_nSize(nSize), m_nPos(0), m_bFailed(false)
{
}

/// @brief 从字符串创建读取器。
CBinaryReader::CBinaryReader(const std::string& strData)
    : m_pData(strData.data()), m_nSize(strData.size()), m_nPos(0), m_bFailed(false)
{
}

/// @brief 读取 nLen 字节到输出；失败时保持位置不变并置失败标记。
bool CBinaryReader::ReadRaw(char* pOut, size_t nLen)
{
    if (m_bFailed || m_nSize - m_nPos < nLen)
    {
        m_bFailed = true;
        return false;
    }
    std::memcpy(pOut, m_pData + m_nPos, nLen);
    m_nPos += nLen;
    return true;
}

/// @brief 读取一个字节。
bool CBinaryReader::ReadU8(std::uint8_t* pOut)
{
    if (pOut == nullptr)
    {
        m_bFailed = true;
        return false;
    }
    char szTmp = 0;
    if (!ReadRaw(&szTmp, 1))
    {
        return false;
    }
    *pOut = static_cast<std::uint8_t>(szTmp);
    return true;
}

/// @brief 读取 16 位无符号整数（小端）。
bool CBinaryReader::ReadU16(std::uint16_t* pOut)
{
    if (pOut == nullptr)
    {
        m_bFailed = true;
        return false;
    }
    char szTmp[2];
    if (!ReadRaw(szTmp, 2))
    {
        return false;
    }
    std::uint16_t nValue = 0;
    for (int i = 0; i < 2; ++i)
    {
        nValue |= static_cast<std::uint16_t>(
            static_cast<std::uint8_t>(szTmp[i])) << (i * 8);
    }
    *pOut = nValue;
    return true;
}

/// @brief 读取 32 位无符号整数（小端）。
bool CBinaryReader::ReadU32(std::uint32_t* pOut)
{
    if (pOut == nullptr)
    {
        m_bFailed = true;
        return false;
    }
    char szTmp[4];
    if (!ReadRaw(szTmp, 4))
    {
        return false;
    }
    std::uint32_t nValue = 0;
    for (int i = 0; i < 4; ++i)
    {
        nValue |= static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(szTmp[i])) << (i * 8);
    }
    *pOut = nValue;
    return true;
}

/// @brief 读取 64 位无符号整数（小端）。
bool CBinaryReader::ReadU64(std::uint64_t* pOut)
{
    if (pOut == nullptr)
    {
        m_bFailed = true;
        return false;
    }
    char szTmp[8];
    if (!ReadRaw(szTmp, 8))
    {
        return false;
    }
    std::uint64_t nValue = 0;
    for (int i = 0; i < 8; ++i)
    {
        nValue |= static_cast<std::uint64_t>(
            static_cast<std::uint8_t>(szTmp[i])) << (i * 8);
    }
    *pOut = nValue;
    return true;
}

/// @brief 读取布尔。
bool CBinaryReader::ReadBool(bool* pOut)
{
    std::uint8_t nValue = 0;
    if (!ReadU8(&nValue))
    {
        return false;
    }
    if (pOut != nullptr)
    {
        *pOut = (nValue != 0);
    }
    return true;
}

/// @brief 读取原始字节串（前 4 字节为长度）。
bool CBinaryReader::ReadBytes(std::string* pOut)
{
    std::uint32_t nLen = 0;
    if (!ReadU32(&nLen))
    {
        return false;
    }
    if (m_bFailed || static_cast<size_t>(nLen) > m_nSize - m_nPos)
    {
        m_bFailed = true;
        return false;
    }
    if (pOut != nullptr)
    {
        pOut->assign(m_pData + m_nPos, nLen);
    }
    m_nPos += nLen;
    return true;
}

/// @brief 读取字符串（前 4 字节为长度）。
bool CBinaryReader::ReadString(std::string* pOut)
{
    return ReadBytes(pOut);
}

/// @brief 已读取位置。
size_t CBinaryReader::Position() const
{
    return m_nPos;
}

/// @brief 剩余可读字节数。
size_t CBinaryReader::Remaining() const
{
    return m_bFailed ? 0 : (m_nSize - m_nPos);
}

/// @brief 是否已读完。
bool CBinaryReader::AtEnd() const
{
    return m_bFailed || m_nPos >= m_nSize;
}

/// @brief 是否处于出错状态。
bool CBinaryReader::Failed() const
{
    return m_bFailed;
}

} // namespace serialization
} // namespace common
