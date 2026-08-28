#include "Serialization/BinaryWriter.h"

#include <cstring>

namespace common {
namespace serialization {

/// @brief 创建二进制写入器。
CBinaryWriter::CBinaryWriter()
{
}

/// @brief 写入一个字节。
void CBinaryWriter::WriteU8(std::uint8_t nValue)
{
    m_buffer.push_back(static_cast<char>(nValue));
}

/// @brief 写入 16 位无符号整数（小端）。
void CBinaryWriter::WriteU16(std::uint16_t nValue)
{
    char szTmp[2];
    szTmp[0] = static_cast<char>(nValue & 0xFF);
    szTmp[1] = static_cast<char>((nValue >> 8) & 0xFF);
    m_buffer.append(szTmp, 2);
}

/// @brief 写入 32 位无符号整数（小端）。
void CBinaryWriter::WriteU32(std::uint32_t nValue)
{
    char szTmp[4];
    for (int i = 0; i < 4; ++i)
    {
        szTmp[i] = static_cast<char>((nValue >> (i * 8)) & 0xFF);
    }
    m_buffer.append(szTmp, 4);
}

/// @brief 写入 64 位无符号整数（小端）。
void CBinaryWriter::WriteU64(std::uint64_t nValue)
{
    char szTmp[8];
    for (int i = 0; i < 8; ++i)
    {
        szTmp[i] = static_cast<char>((nValue >> (i * 8)) & 0xFF);
    }
    m_buffer.append(szTmp, 8);
}

/// @brief 写入布尔（编码为 1 字节 0/1）。
void CBinaryWriter::WriteBool(bool bValue)
{
    WriteU8(bValue ? 1 : 0);
}

/// @brief 写入原始字节串（前 4 字节为长度，小端）。
void CBinaryWriter::WriteBytes(const char* pData, size_t nLen)
{
    WriteU32(static_cast<std::uint32_t>(nLen));
    if (nLen > 0)
    {
        m_buffer.append(pData, nLen);
    }
}

/// @brief 写入字符串（前 4 字节为字节长度，小端）。
void CBinaryWriter::WriteString(const std::string& strValue)
{
    WriteBytes(strValue.data(), strValue.size());
}

/// @brief 已写入的总字节数。
size_t CBinaryWriter::Size() const
{
    return m_buffer.size();
}

/// @brief 是否为空。
bool CBinaryWriter::Empty() const
{
    return m_buffer.empty();
}

/// @brief 当前缓冲内容（借用引用）。
const std::string& CBinaryWriter::Data() const
{
    return m_buffer;
}

/// @brief 释放并返回缓冲内容。
std::string CBinaryWriter::Take()
{
    std::string strResult;
    strResult.swap(m_buffer);
    return strResult;
}

/// @brief 清空缓冲。
void CBinaryWriter::Clear()
{
    m_buffer.clear();
}

} // namespace serialization
} // namespace common
