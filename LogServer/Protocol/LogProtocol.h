#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Message/IMessageRouter.h"

namespace logserver {

/// @brief 日志协议命令。
enum LogCommand : std::uint8_t
{
    kCmdSubmitLog = 1, // 上报日志
    kCmdPing = 2,      // 心跳
    kCmdPong = 3,      // 心跳响应
};

/// @brief 单条日志记录。
struct LogRecord
{
    std::uint64_t nTimestamp; // 时间戳（epoch 秒，由上报方提供）
    std::string strLevel;     // 级别（trace/debug/info/warn/error）
    std::string strSource;    // 来源（服务器名，不含 '|'）
    std::string strContent;   // 内容（编码时作为最后一个字段，可含 '|'）
};

/// @brief 报文解析状态。
enum class ParseStatus : int
{
    kNeedMore = 0, // 数据不足，等待更多数据
    kOk = 1,       // 解析成功
    kInvalid = 2,  // 协议格式非法
};

/// @brief 报文解析输出（结构体收敛，避免 C 风格 out 指针）。
///
/// 仅在 status == kOk 时 command / payload / consumed 有效。
struct Packet
{
    ParseStatus status;     // 解析状态
    std::uint8_t command;   // 命令（kOk 时有效）
    std::string payload;    // 负载（kOk 时有效）
    size_t consumed;        // 本报文占用的字节数（kOk 时有效）
};

/// @brief 日志上报协议。
///
/// 报文格式（网络字节序）：
/// +------------+------------+-----------------+
/// | Length(4B) | Command(1B)| Payload(N B)    |
/// +------------+------------+-----------------+
///
/// Length = Command + Payload 的总字节数，即 1 + N。
/// kCmdSubmitLog 的 Payload 为文本，格式：
///   <epoch秒>|<level>|<source>|<content>
/// content 为最后一个字段，可包含 '|'。
/// 协议属于 LogServer，不属于 ServerCore。
class CLogProtocol
{
public:
    // 头部长度。
    static const size_t kHeaderSize = 4;

    // 最大报文长度，防止恶意超长报文。
    static const size_t kMaxPacketSize = 1 * 1024 * 1024;

    // 从数据流起始处解析一个完整报文。
    static Packet ParsePacket(const std::string& strData);

    // 构建报文。
    static std::string BuildPacket(std::uint8_t nCommand, const std::string& strPayload);

    // 编码日志记录为 kCmdSubmitLog 负载文本。
    static std::string EncodeRecord(const LogRecord& record);

    // 解码 kCmdSubmitLog 负载文本为日志记录。
    static bool DecodeRecord(const std::string& strPayload, LogRecord* pRecord);

    // 构建日志上报报文。
    static std::string BuildSubmit(const LogRecord& record);

    // 构建 PING 请求。
    static std::string BuildPing();

    // 构建 PONG 响应。
    static std::string BuildPong();

    // 生成 ServerCore 消息提取器（供 CMessageRouter 使用）。
    // 负载为借用指针，指向输入缓冲内部，仅在提取后立即分发时有效。
    static sc::MessageExtractor MakeMessageExtractor();
};

} // namespace logserver
