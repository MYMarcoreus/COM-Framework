/// @file test_serialization.cpp
/// Common 序列化基础设施单元测试（CBinaryWriter / CBinaryReader）。

#include <cstdint>
#include <string>

#include "TestFramework.h"

#include "Serialization/BinaryReader.h"
#include "Serialization/BinaryWriter.h"

namespace {

/// @brief 写读往返：各字段类型一致。\n
TEST(Serialization_RoundTrip)
{
    common::CBinaryWriter writer;
    writer.WriteU8(0xAB);
    writer.WriteU16(0x1234);
    writer.WriteU32(0xDEADBEEF);
    writer.WriteU64(0x0102030405060708ULL);
    writer.WriteBool(true);
    writer.WriteString("hello");
    writer.WriteBytes("raw", 3);

    common::CBinaryReader reader(writer.Data());
    std::uint8_t u8 = 0;
    std::uint16_t u16 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    bool b = false;
    std::string str;
    std::string bytes;
    ASSERT_TRUE(reader.ReadU8(&u8));
    ASSERT_TRUE(reader.ReadU16(&u16));
    ASSERT_TRUE(reader.ReadU32(&u32));
    ASSERT_TRUE(reader.ReadU64(&u64));
    ASSERT_TRUE(reader.ReadBool(&b));
    ASSERT_TRUE(reader.ReadString(&str));
    ASSERT_TRUE(reader.ReadBytes(&bytes));

    ASSERT_TRUE(u8 == 0xAB);
    ASSERT_TRUE(u16 == 0x1234);
    ASSERT_TRUE(u32 == 0xDEADBEEF);
    ASSERT_TRUE(u64 == 0x0102030405060708ULL);
    ASSERT_TRUE(b);
    ASSERT_TRUE(str == "hello");
    ASSERT_TRUE(bytes == "raw");
    ASSERT_TRUE(reader.AtEnd());
    ASSERT_TRUE(!reader.Failed());
}

/// @brief 字节序：U16 小端编码验证。\n
TEST(Serialization_LittleEndian)
{
    common::CBinaryWriter writer;
    writer.WriteU16(0x0102);
    writer.WriteU32(0x01020304);
    const std::string& data = writer.Data();
    ASSERT_TRUE(data.size() == 6);
    // 小端：低字节在前
    ASSERT_TRUE(static_cast<unsigned char>(data[0]) == 0x02);
    ASSERT_TRUE(static_cast<unsigned char>(data[1]) == 0x01);
    ASSERT_TRUE(static_cast<unsigned char>(data[2]) == 0x04);
    ASSERT_TRUE(static_cast<unsigned char>(data[3]) == 0x03);
}

/// @brief 边界检查：数据不足时读取失败并保持位置不变。\n
TEST(Serialization_BoundaryCheck)
{
    // 只有 2 字节，读 U32 应失败
    const char szData[2] = { '\x01', '\x02' };
    common::CBinaryReader reader(szData, 2);
    std::uint32_t u32 = 0;
    ASSERT_TRUE(!reader.ReadU32(&u32));
    ASSERT_TRUE(reader.Failed());
    ASSERT_TRUE(reader.Remaining() == 0);
}

/// @brief 空字符串 / 空字节串往返。\n
TEST(Serialization_EmptyString)
{
    common::CBinaryWriter writer;
    writer.WriteString("");
    writer.WriteBytes(nullptr, 0);

    common::CBinaryReader reader(writer.Data());
    std::string str;
    std::string bytes;
    ASSERT_TRUE(reader.ReadString(&str));
    ASSERT_TRUE(reader.ReadBytes(&bytes));
    ASSERT_TRUE(str.empty());
    ASSERT_TRUE(bytes.empty());
}

} // namespace
