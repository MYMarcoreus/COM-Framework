#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace common {

/// @brief 二进制写入器（小端字节序）。
///
/// 用于构建二进制协议负载 / 报文：向内存缓冲顺序写入各字段，
/// 统一按小端编码并做长度前缀（字符串 / 字节串）。
/// 与 CBinaryReader 配套；不做网络序转换（小端机器占主导，统一约定小端）。
class CBinaryWriter
{
public:
    CBinaryWriter();

    // 写入一个字节。
    void WriteU8(std::uint8_t nValue);

    // 写入 16 位无符号整数（小端）。
    void WriteU16(std::uint16_t nValue);

    // 写入 32 位无符号整数（小端）。
    void WriteU32(std::uint32_t nValue);

    // 写入 64 位无符号整数（小端）。
    void WriteU64(std::uint64_t nValue);

    // 写入布尔（编码为 1 字节 0/1）。
    void WriteBool(bool bValue);

    // 写入原始字节串（前 4 字节为长度，小端）。
    void WriteBytes(const char* pData, size_t nLen);

    // 写入字符串（前 4 字节为字节长度，小端）。
    void WriteString(const std::string& strValue);

    // 已写入的总字节数。
    size_t Size() const;

    // 是否为空。
    bool Empty() const;

    // 当前缓冲内容（借用引用，仅本次有效）。
    const std::string& Data() const;

    // 释放并返回缓冲内容。
    std::string Take();

    // 清空缓冲。
    void Clear();

private:
    std::string m_buffer;
};

} // namespace common
