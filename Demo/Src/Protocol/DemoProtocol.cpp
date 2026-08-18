#include "Protocol/DemoProtocol.h"

#include <arpa/inet.h>
#include <cstring>

namespace demo {

/// @brief 从数据流中解析一个完整报文。
///
/// 处理半包与粘包：数据不足时返回 kNeedMore 并等待更多数据；
/// 解析成功后从 consumed 返回本报文占用字节数。
///
/// @param data 待解析的数据流。
/// @param consumed 输出本报文占用的字节数（解析成功时）。
/// @param packet 输出解析得到的报文。
///
/// @return 解析结果。
ParseResult CDemoProtocol::ParsePacket(const std::string& data, size_t* consumed, Packet* packet)
{
    if (consumed == nullptr || packet == nullptr)
    {
        return ParseResult::kInvalid;
    }
    *consumed = 0;
    if (data.size() < kHeaderSize)
    {
        return ParseResult::kNeedMore; // 头部不完整
    }
    // ① 读取长度字段（网络字节序）
    std::uint32_t len = 0;
    std::memcpy(&len, data.data(), sizeof(len));
    len = ntohl(len);
    if (len < 1)
    {
        return ParseResult::kInvalid; // 非法长度（至少包含 Command）
    }
    if (len > kMaxPacketSize)
    {
        return ParseResult::kInvalid; // 超长报文，拒绝
    }
    // ② 检查完整报文是否到达
    size_t total = kHeaderSize + len;
    if (data.size() < total)
    {
        return ParseResult::kNeedMore; // 半包
    }
    // ③ 解析命令与负载
    packet->command = static_cast<std::uint8_t>(data[kHeaderSize]);
    packet->payload.assign(data, kHeaderSize + 1, len - 1);
    *consumed = total;
    return ParseResult::kOk;
}

/// @brief 构建报文。
///
/// @param command 命令。
/// @param payload 负载。
///
/// @return 编码后的原始字节。
std::string CDemoProtocol::BuildPacket(std::uint8_t command, const std::string& payload)
{
    std::uint32_t len = static_cast<std::uint32_t>(1 + payload.size());
    std::string out;
    out.reserve(kHeaderSize + len);
    std::uint32_t netLen = htonl(len);
    out.append(reinterpret_cast<const char*>(&netLen), sizeof(netLen));
    out.push_back(static_cast<char>(command));
    out.append(payload);
    return out;
}

/// @brief 构建 PING 请求。
std::string CDemoProtocol::BuildPing()
{
    return BuildPacket(kCmdPing, "ping");
}

/// @brief 构建 PONG 响应。
std::string CDemoProtocol::BuildPong()
{
    return BuildPacket(kCmdPong, "pong");
}

} // namespace demo
