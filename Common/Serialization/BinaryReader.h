#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace common {
namespace serialization {

/// @brief 二进制读取器（小端字节序，带边界检查）。
///
/// 从内存缓冲顺序读取各字段，与 CBinaryWriter 配套。
/// 所有读取均检查剩余字节，数据不足时返回 false 并保持位置不变
/// （调用方可决定是否丢弃整条消息）。
class CBinaryReader
{
public:
    // 从缓冲创建读取器（借用指针，读取期间缓冲必须保持存活）。
    CBinaryReader(const char* pData, size_t nSize);

    // 从字符串创建读取器（借用引用）。
    explicit CBinaryReader(const std::string& strData);

    // 读取一个字节。
    bool ReadU8(std::uint8_t* pOut);

    // 读取 16 位无符号整数（小端）。
    bool ReadU16(std::uint16_t* pOut);

    // 读取 32 位无符号整数（小端）。
    bool ReadU32(std::uint32_t* pOut);

    // 读取 64 位无符号整数（小端）。
    bool ReadU64(std::uint64_t* pOut);

    // 读取布尔。
    bool ReadBool(bool* pOut);

    // 读取原始字节串（前 4 字节为长度，小端）。
    bool ReadBytes(std::string* pOut);

    // 读取字符串（前 4 字节为字节长度，小端）。
    bool ReadString(std::string* pOut);

    // 已读取位置。
    size_t Position() const;

    // 剩余可读字节数。
    size_t Remaining() const;

    // 是否已读完（无剩余）。
    bool AtEnd() const;

    // 是否处于出错状态（任一次读取失败）。
    bool Failed() const;

private:
    // 读取 nLen 字节到输出（失败时回滚位置）。
    bool ReadRaw(char* pOut, size_t nLen);

    const char* m_pData;
    size_t m_nSize;
    size_t m_nPos;
    bool m_bFailed;
};

} // namespace serialization
} // namespace common
