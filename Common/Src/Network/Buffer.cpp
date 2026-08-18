#include "Network/Buffer.h"

#include <cstring>

namespace common {

/// @brief 创建网络缓冲区。
///
/// @param initialSize 初始容量。
CBuffer::CBuffer(size_t initialSize)
    : m_vecData(initialSize), m_nReadIndex(0), m_nWriteIndex(0)
{
}

/// @brief 追加数据到缓冲区可写区。
///
/// @param pData 数据起始指针。
/// @param nLen 数据长度。
void CBuffer::Append(const char* pData, size_t nLen)
{
    EnsureWritable(nLen);
    std::memcpy(BeginWrite(), pData, nLen);
    m_nWriteIndex += nLen;
}

/// @brief 追加字符串到缓冲区可写区。
///
/// @param str 待追加字符串。
void CBuffer::Append(const std::string& str)
{
    Append(str.data(), str.size());
}

/// @brief 返回可读字节数。
size_t CBuffer::Readable() const
{
    return m_nWriteIndex - m_nReadIndex;
}

/// @brief 返回可写字节数。
size_t CBuffer::Writable() const
{
    return m_vecData.size() - m_nWriteIndex;
}

/// @brief 返回可读区起始指针。
const char* CBuffer::Peek() const
{
    return &m_vecData[m_nReadIndex];
}

/// @brief 返回可写区起始指针。
char* CBuffer::BeginWrite()
{
    return &m_vecData[m_nWriteIndex];
}

/// @brief 消费可读区前 nLen 字节。
///
/// @param nLen 要消费的字节数。
void CBuffer::Retrieve(size_t nLen)
{
    if (nLen >= Readable())
    {
        RetrieveAll();
        return;
    }
    m_nReadIndex += nLen;
}

/// @brief 清空缓冲区。
void CBuffer::RetrieveAll()
{
    m_nReadIndex = 0;
    m_nWriteIndex = 0;
}

/// @brief 将可读数据转为字符串。
std::string CBuffer::ToString() const
{
    return std::string(Peek(), Readable());
}

/// @brief 返回当前容量。
size_t CBuffer::Capacity() const
{
    return m_vecData.size();
}

/// @brief 确保可写空间不小于 nLen。
///
/// ① 可写空间足够时直接返回。
/// ② 整理已消费空间后够用时搬移数据。
/// ③ 否则扩容。
///
/// @param nLen 需要的可写空间。
void CBuffer::EnsureWritable(size_t nLen)
{
    // ① 可写空间足够
    if (Writable() >= nLen)
    {
        return;
    }
    // ② 整理已消费空间
    if (m_nReadIndex + Writable() >= nLen)
    {
        std::memmove(&m_vecData[0], Peek(), Readable());
        m_nWriteIndex = Readable();
        m_nReadIndex = 0;
        return;
    }
    // ③ 扩容
    size_t nNewSize = m_vecData.size() * 2;
    while (nNewSize - m_nWriteIndex < nLen)
    {
        nNewSize *= 2;
    }
    std::vector<char> vecNewData(nNewSize);
    std::memcpy(&vecNewData[0], Peek(), Readable());
    m_vecData.swap(vecNewData);
    m_nWriteIndex = Readable();
    m_nReadIndex = 0;
}

} // namespace common
