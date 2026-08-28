#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace common {
namespace network {

/// @brief 网络数据缓冲区。
///
/// 基于 std::vector 的读写缓冲区，支持追加、读取、回收与扩容。
class CBuffer
{
public:
    // 默认初始容量。
    static const size_t kInitialSize = 1024;

    explicit CBuffer(size_t initialSize = kInitialSize);

    // 追加数据到可写区。
    void Append(const char* pData, size_t nLen);

    // 追加字符串到可写区。
    void Append(const std::string& str);

    // 可读字节数。
    size_t Readable() const;

    // 可写字节数。
    size_t Writable() const;

    // 指向可读区起始位置。
    const char* Peek() const;

    // 指向可写区起始位置。
    char* BeginWrite();

    // 消费可读区前 nLen 字节。
    void Retrieve(size_t nLen);

    // 清空缓冲区。
    void RetrieveAll();

    // 将可读数据转为字符串。
    std::string ToString() const;

    // 当前容量。
    size_t Capacity() const;

private:
    // 确保可写空间不小于 len。
    void EnsureWritable(size_t len);

    std::vector<char> m_vecData;
    size_t m_nReadIndex;
    size_t m_nWriteIndex;
};

} // namespace network
} // namespace common
