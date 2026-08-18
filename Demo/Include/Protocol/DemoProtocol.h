#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace demo {

/// @brief Demo 协议命令。
enum DemoCommand : std::uint8_t
{
    kCmdPing = 1, // 心跳
    kCmdEcho = 2, // 回显
    kCmdPong = 3, // 心跳响应
};

/// @brief Demo 协议报文。
struct Packet
{
    std::uint8_t command; // 命令
    std::string payload;  // 负载
};

/// @brief 报文解析结果。
enum class ParseResult : int
{
    kNeedMore = 0, // 数据不足，等待更多数据
    kOk = 1,       // 解析成功
    kInvalid = 2,  // 协议格式非法
};

/// @brief Demo 极简通信协议。
///
/// 报文格式（网络字节序）：
/// +------------+------------+-----------------+
/// | Length(4B) | Command(1B)| Payload(N B)    |
/// +------------+------------+-----------------+
///
/// Length = Command + Payload 的总字节数，即 1 + N。
/// 协议只用于验证 ServerCore 网络能力，不属于 ServerCore。
class DemoProtocol
{
public:
    // 头部长度。
    static const size_t kHeaderSize = 4;

    // 最大报文长度，防止恶意超长报文。
    static const size_t kMaxPacketSize = 1 * 1024 * 1024;

    // 从数据流中解析一个完整报文。
    static ParseResult ParsePacket(const std::string& data, size_t* consumed, Packet* packet);

    // 构建报文。
    static std::string BuildPacket(std::uint8_t command, const std::string& payload);

    // 构建 PING 请求。
    static std::string BuildPing();

    // 构建 PONG 响应。
    static std::string BuildPong();
};

} // namespace demo
