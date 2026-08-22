#include "Protocol/LogProtocol.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>

namespace logserver {

/// @brief 从数据流起始处解析一个完整报文。
///
/// 处理半包与粘包：数据不足时返回 kNeedMore 并等待更多数据；
/// 解析成功时 consumed 给出本报文占用的字节数。
///
/// @param strData 待解析的数据流。
///
/// @return 解析结果（结构体收敛输出）。
Packet CLogProtocol::ParsePacket(const std::string& strData)
{
    Packet packet;
    packet.status = ParseStatus::kNeedMore;
    packet.command = 0;
    packet.consumed = 0;
    if (strData.size() < kHeaderSize)
    {
        return packet; // 头部不完整
    }
    // ① 读取长度字段（网络字节序）
    std::uint32_t nLen = 0;
    std::memcpy(&nLen, strData.data(), sizeof(nLen));
    nLen = ntohl(nLen);
    if (nLen < 1)
    {
        packet.status = ParseStatus::kInvalid; // 非法长度（至少包含 Command）
        return packet;
    }
    if (nLen > kMaxPacketSize)
    {
        packet.status = ParseStatus::kInvalid; // 超长报文，拒绝
        return packet;
    }
    // ② 检查完整报文是否到达
    size_t nTotal = kHeaderSize + nLen;
    if (strData.size() < nTotal)
    {
        return packet; // 半包
    }
    // ③ 解析命令与负载
    packet.command = static_cast<std::uint8_t>(strData[kHeaderSize]);
    packet.payload.assign(strData, kHeaderSize + 1, nLen - 1);
    packet.consumed = nTotal;
    packet.status = ParseStatus::kOk;
    return packet;
}

/// @brief 构建报文。
///
/// @param nCommand   命令。
/// @param strPayload 负载。
///
/// @return 编码后的原始字节。
std::string CLogProtocol::BuildPacket(std::uint8_t nCommand, const std::string& strPayload)
{
    std::uint32_t nLen = static_cast<std::uint32_t>(1 + strPayload.size());
    std::string strOut;
    strOut.reserve(kHeaderSize + nLen);
    std::uint32_t nNetLen = htonl(nLen);
    strOut.append(reinterpret_cast<const char*>(&nNetLen), sizeof(nNetLen));
    strOut.push_back(static_cast<char>(nCommand));
    strOut.append(strPayload);
    return strOut;
}

/// @brief 编码日志记录为 kCmdSubmitLog 负载文本。
///
/// 格式：<epoch秒>|<level>|<source>|<content>。
/// content 为最后一个字段，可包含 '|'。
///
/// @param record 日志记录。
///
/// @return 编码后的负载文本。
std::string CLogProtocol::EncodeRecord(const LogRecord& record)
{
    std::string strOut;
    strOut.reserve(64 + record.strSource.size() + record.strContent.size());
    strOut += std::to_string(record.nTimestamp);
    strOut += '|';
    strOut += record.strLevel;
    strOut += '|';
    strOut += record.strSource;
    strOut += '|';
    strOut += record.strContent;
    return strOut;
}

/// @brief 解码 kCmdSubmitLog 负载文本为日志记录。
///
/// 只按前三个 '|' 切分字段，content 为剩余部分（可含 '|'）。
///
/// @param strPayload 负载文本。
/// @param pRecord    输出日志记录。
///
/// @return true 解码成功；false 字段数不足或时间戳非法。
bool CLogProtocol::DecodeRecord(const std::string& strPayload, LogRecord* pRecord)
{
    if (pRecord == nullptr)
    {
        return false;
    }
    // ① 逐字段切分（最多 4 段，content 取剩余全部）
    std::string astrFields[4];
    size_t nField = 0;
    size_t nStart = 0;
    for (size_t i = 0; i < strPayload.size() && nField < 3; ++i)
    {
        if (strPayload[i] == '|')
        {
            astrFields[nField++] = strPayload.substr(nStart, i - nStart);
            nStart = i + 1;
        }
    }
    if (nField < 3)
    {
        return false; // 字段不足：timestamp|level|source|content
    }
    astrFields[3] = strPayload.substr(nStart);

    // ② 解析时间戳
    char* pEnd = nullptr;
    std::uint64_t nTimestamp = std::strtoull(astrFields[0].c_str(), &pEnd, 10);
    if (pEnd == astrFields[0].c_str() || *pEnd != '\0')
    {
        return false; // 时间戳非法
    }

    pRecord->nTimestamp = nTimestamp;
    pRecord->strLevel = astrFields[1];
    pRecord->strSource = astrFields[2];
    pRecord->strContent = astrFields[3];
    return true;
}

/// @brief 构建日志上报报文。
///
/// @param record 日志记录。
///
/// @return 编码后的原始字节。
std::string CLogProtocol::BuildSubmit(const LogRecord& record)
{
    return BuildPacket(kCmdSubmitLog, EncodeRecord(record));
}

/// @brief 构建 PING 请求。
std::string CLogProtocol::BuildPing()
{
    return BuildPacket(kCmdPing, "ping");
}

/// @brief 构建 PONG 响应。
std::string CLogProtocol::BuildPong()
{
    return BuildPacket(kCmdPong, "pong");
}

/// @brief 生成 ServerCore 消息提取器。
///
/// 直接解析输入缓冲（零拷贝，负载借用缓冲内部指针），逻辑与 ParsePacket 一致。
///
/// @return 消息提取器（可重复调用，线程安全）。
sc::MessageExtractor CLogProtocol::MakeMessageExtractor()
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

} // namespace logserver
