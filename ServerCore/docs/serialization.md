# 序列化（Common/Serialization）

Common 提供轻量二进制序列化基础设施（小端字节序，带边界检查），
用于构建 / 解析二进制协议负载或持久化数据。

## CBinaryWriter

内存写入缓冲，支持：

| 方法 | 编码 |
|---|---|
| `WriteU8 / WriteU16 / WriteU32 / WriteU64` | 定长小端 |
| `WriteBool` | 1 字节 0/1 |
| `WriteString / WriteBytes` | 4 字节长度前缀 + 原始字节（小端） |
| `Data() / Take() / Size() / Empty() / Clear()` | 缓冲访问 |

```cpp
common::CBinaryWriter writer;
writer.WriteU8(0xAB);
writer.WriteU32(0xDEADBEEF);
writer.WriteString("hello");
writer.WriteBytes("raw", 3);

std::string payload = writer.Take();   // 移出缓冲
```

## CBinaryReader

顺序读取，**带边界检查**：数据不足时返回 `false` 并置 `Failed()` 标记
（位置保持不动），调用方可决定丢弃整条消息。

```cpp
common::CBinaryReader reader(payload);
std::uint8_t u8; std::uint32_t u32; std::string str; std::string bytes;
if (!reader.ReadU8(&u8))  return;      // 边界失败
if (!reader.ReadU32(&u32)) return;
if (!reader.ReadString(&str)) return;
if (!reader.ReadBytes(&bytes)) return;
```

## 约定

- **小端字节序**（与主流 x86/ARM 一致；如需要网络序请自行转换）。
- 字符串 / 字节串统一 `uint32` 长度前缀，防止歧义。
- 读者借用输入缓冲，读取期间缓冲必须保持存活。
- 空字符串 / 空字节串合法（长度为 0）。

## 测试

`Tests/test_serialization.cpp` 覆盖往返、字节序、边界、空串场景。

## 与协议的关系

序列化是字段级编码工具；完整报文（长度头 + 命令 + 负载）的切分由
[消息流水线](messaging.md) 的提取器完成。两者可组合使用：
提取器切出负载 → 负载内用 `CBinaryReader` 解字段。
