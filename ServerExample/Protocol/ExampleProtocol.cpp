#include "Protocol/ExampleProtocol.h"

#include <arpa/inet.h>
#include <cstring>

namespace serverexample {

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
ParseResult CExampleProtocol::ParsePacket(const std::string& data, size_t* consumed, Packet* packet)
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
std::string CExampleProtocol::BuildPacket(std::uint8_t command, const std::string& payload)
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
std::string CExampleProtocol::BuildPing()
{
    return BuildPacket(kCmdPing, "ping");
}

/// @brief 构建 PONG 响应。
std::string CExampleProtocol::BuildPong()
{
    return BuildPacket(kCmdPong, "pong");
}

/// @brief 生成 ServerCore 消息提取器。
///
/// 直接解析输入缓冲（零拷贝，负载借用缓冲内部指针），逻辑与 ParsePacket 一致。
///
/// @return 消息提取器（可重复调用，线程安全）。
sc::MessageExtractor CExampleProtocol::MakeMessageExtractor()
{
    return [](const char* pData, size_t nLen) -> sc::ExtractedMessage
    {
        sc::ExtractedMessage result;
        result.result = sc::MessageParseResult::kNeedMore;
        result.step = 0;
        result.type = 0;
        result.payload = nullptr;
        result.payloadSize = 0;
        if (pData == nullptr || nLen < kHeaderSize)
        {
            return result; // 头部不完整，等待更多数据
        }
        // ① 读取长度字段（网络字节序）
        std::uint32_t nLenField = 0;
        std::memcpy(&nLenField, pData, sizeof(nLenField));
        nLenField = ntohl(nLenField);
        if (nLenField < 1 || nLenField > kMaxPacketSize)
        {
            result.result = sc::MessageParseResult::kInvalid; // 非法长度
            return result;
        }
        // ② 检查完整报文是否到达
        size_t nTotal = kHeaderSize + nLenField;
        if (nLen < nTotal)
        {
            return result; // 半包，等待更多数据
        }
        // ③ 借用输入缓冲内部指针返回负载
        result.result = sc::MessageParseResult::kOk;
        result.step = nTotal;
        result.type = static_cast<int>(static_cast<std::uint8_t>(pData[kHeaderSize]));
        result.payload = pData + kHeaderSize + 1;
        result.payloadSize = nLenField - 1;
        return result;
    };
}

} // namespace serverexample
