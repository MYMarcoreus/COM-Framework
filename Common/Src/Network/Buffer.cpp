#include "Network/Buffer.h"

#include <cstring>

namespace common {

/// @brief 创建网络缓冲区。
///
/// @param initialSize 初始容量。
Buffer::Buffer(size_t initialSize)
    : data_(initialSize), readIndex_(0), writeIndex_(0)
{
}

/// @brief 追加数据到缓冲区可写区。
///
/// @param data 数据起始指针。
/// @param len 数据长度。
void Buffer::Append(const char* data, size_t len)
{
    EnsureWritable(len);
    std::memcpy(BeginWrite(), data, len);
    writeIndex_ += len;
}

/// @brief 追加字符串到缓冲区可写区。
///
/// @param str 待追加字符串。
void Buffer::Append(const std::string& str)
{
    Append(str.data(), str.size());
}

/// @brief 返回可读字节数。
size_t Buffer::Readable() const
{
    return writeIndex_ - readIndex_;
}

/// @brief 返回可写字节数。
size_t Buffer::Writable() const
{
    return data_.size() - writeIndex_;
}

/// @brief 返回可读区起始指针。
const char* Buffer::Peek() const
{
    return &data_[readIndex_];
}

/// @brief 返回可写区起始指针。
char* Buffer::BeginWrite()
{
    return &data_[writeIndex_];
}

/// @brief 消费可读区前 len 字节。
///
/// @param len 要消费的字节数。
void Buffer::Retrieve(size_t len)
{
    if (len >= Readable())
    {
        RetrieveAll();
        return;
    }
    readIndex_ += len;
}

/// @brief 清空缓冲区。
void Buffer::RetrieveAll()
{
    readIndex_ = 0;
    writeIndex_ = 0;
}

/// @brief 将可读数据转为字符串。
std::string Buffer::ToString() const
{
    return std::string(Peek(), Readable());
}

/// @brief 返回当前容量。
size_t Buffer::Capacity() const
{
    return data_.size();
}

/// @brief 确保可写空间不小于 len。
///
/// ① 可写空间足够时直接返回。
/// ② 整理已消费空间后够用时搬移数据。
/// ③ 否则扩容。
///
/// @param len 需要的可写空间。
void Buffer::EnsureWritable(size_t len)
{
    // ① 可写空间足够
    if (Writable() >= len)
    {
        return;
    }
    // ② 整理已消费空间
    if (readIndex_ + Writable() >= len)
    {
        std::memmove(&data_[0], Peek(), Readable());
        writeIndex_ = Readable();
        readIndex_ = 0;
        return;
    }
    // ③ 扩容
    size_t newSize = data_.size() * 2;
    while (newSize - writeIndex_ < len)
    {
        newSize *= 2;
    }
    std::vector<char> newData(newSize);
    std::memcpy(&newData[0], Peek(), Readable());
    data_.swap(newData);
    writeIndex_ = Readable();
    readIndex_ = 0;
}

} // namespace common
