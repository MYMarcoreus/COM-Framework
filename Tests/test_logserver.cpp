/// @file test_logserver.cpp
/// LogServer 业务单元测试：日志协议编解码 / 日志存储落盘。
///
/// 覆盖半包 / 粘包 / 非法报文 / 来源清理等边界，验证业务模块逻辑。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "TestFramework.h"

#include "Protocol/LogProtocol.h"
#include "Service/LogStorage.h"

namespace {

/// @brief 生成唯一临时目录路径。
std::string TempDir()
{
    return std::string("/tmp/logserver_test_") +
           std::to_string(static_cast<long long>(::getpid()));
}

/// @brief 读取文件内容；文件不存在返回空串。
std::string ReadFile(const std::string& strPath)
{
    FILE* pFile = std::fopen(strPath.c_str(), "rb");
    if (pFile == nullptr)
    {
        return "";
    }
    std::string strContent;
    char szBuf[256];
    size_t nRead = 0;
    while ((nRead = std::fread(szBuf, 1, sizeof(szBuf), pFile)) > 0)
    {
        strContent.append(szBuf, nRead);
    }
    std::fclose(pFile);
    return strContent;
}

/// @brief 构造一条测试日志记录。
logserver::LogRecord MakeRecord(const std::string& strLevel, const std::string& strSource,
                                const std::string& strContent)
{
    logserver::LogRecord record;
    record.nTimestamp = 1700000000;
    record.strLevel = strLevel;
    record.strSource = strSource;
    record.strContent = strContent;
    return record;
}

} // namespace

/// @brief 协议：完整报文解析与往返编解码。
TEST(LogProtocol_ParseComplete)
{
    logserver::LogRecord record = MakeRecord("info", "example", "hello");

    std::string strPacket = logserver::CLogProtocol::BuildSubmit(record);
    logserver::Packet packet = logserver::CLogProtocol::ParsePacket(strPacket);
    ASSERT_EQ(packet.status, logserver::ParseStatus::kOk);
    ASSERT_EQ(static_cast<int>(packet.command), static_cast<int>(logserver::kCmdSubmitLog));
    ASSERT_EQ(packet.consumed, strPacket.size());

    logserver::LogRecord decoded;
    ASSERT_TRUE(logserver::CLogProtocol::DecodeRecord(packet.payload, &decoded));
    ASSERT_EQ(decoded.nTimestamp, record.nTimestamp);
    ASSERT_EQ(decoded.strLevel, std::string("info"));
    ASSERT_EQ(decoded.strSource, std::string("example"));
    ASSERT_EQ(decoded.strContent, std::string("hello"));
}

/// @brief 协议：半包与粘包处理。
TEST(LogProtocol_HalfAndSticky)
{
    logserver::LogRecord record = MakeRecord("warn", "example", "sticky");
    std::string strPacket = logserver::CLogProtocol::BuildSubmit(record);

    // 半包：只给头部的一部分，应返回 kNeedMore
    std::string strHalf = strPacket.substr(0, 2);
    logserver::Packet packet = logserver::CLogProtocol::ParsePacket(strHalf);
    ASSERT_EQ(packet.status, logserver::ParseStatus::kNeedMore);

    // 粘包：两个报文连在一起，应解析出第一个且 consumed 恰为本报文长度
    std::string strSticky = strPacket + strPacket;
    packet = logserver::CLogProtocol::ParsePacket(strSticky);
    ASSERT_EQ(packet.status, logserver::ParseStatus::kOk);
    ASSERT_EQ(packet.consumed, strPacket.size());
}

/// @brief 协议：非法报文（长度非法 / 超长）。
TEST(LogProtocol_Invalid)
{
    // 长度字段 < 1（全零）
    std::string strBad(4, '\0');
    logserver::Packet packet = logserver::CLogProtocol::ParsePacket(strBad);
    ASSERT_EQ(packet.status, logserver::ParseStatus::kInvalid);

    // 长度字段 > 上限（拒绝超长报文）
    std::uint32_t nHuge = logserver::CLogProtocol::kMaxPacketSize + 1;
    std::string strHuge;
    strHuge.append(reinterpret_cast<const char*>(&nHuge), sizeof(nHuge));
    packet = logserver::CLogProtocol::ParsePacket(strHuge);
    ASSERT_EQ(packet.status, logserver::ParseStatus::kInvalid);
}

/// @brief 协议：负载解码（content 含 '|' / 字段不足 / 时间戳非法）。
TEST(LogProtocol_DecodeRecord)
{
    // content 是最后一个字段，可含 '|'
    logserver::LogRecord record = MakeRecord("error", "svc", "a|b|c");
    std::string strPayload = logserver::CLogProtocol::EncodeRecord(record);
    logserver::LogRecord decoded;
    ASSERT_TRUE(logserver::CLogProtocol::DecodeRecord(strPayload, &decoded));
    ASSERT_EQ(decoded.nTimestamp, static_cast<std::uint64_t>(1700000000));
    ASSERT_EQ(decoded.strLevel, std::string("error"));
    ASSERT_EQ(decoded.strSource, std::string("svc"));
    ASSERT_EQ(decoded.strContent, std::string("a|b|c"));

    // 字段不足（缺 content）
    ASSERT_TRUE(!logserver::CLogProtocol::DecodeRecord("123|info|svc", &decoded));

    // 时间戳非法
    ASSERT_TRUE(!logserver::CLogProtocol::DecodeRecord("abc|info|svc|content", &decoded));
}

/// @brief 协议：心跳请求 / 响应。
TEST(LogProtocol_PingPong)
{
    std::string strPing = logserver::CLogProtocol::BuildPing();
    logserver::Packet packet = logserver::CLogProtocol::ParsePacket(strPing);
    ASSERT_EQ(packet.status, logserver::ParseStatus::kOk);
    ASSERT_EQ(static_cast<int>(packet.command), static_cast<int>(logserver::kCmdPing));

    std::string strPong = logserver::CLogProtocol::BuildPong();
    packet = logserver::CLogProtocol::ParsePacket(strPong);
    ASSERT_EQ(packet.status, logserver::ParseStatus::kOk);
    ASSERT_EQ(static_cast<int>(packet.command), static_cast<int>(logserver::kCmdPong));
}

/// @brief 存储：按来源分文件落盘。
TEST(LogStorage_WriteBySource)
{
    std::string strDir = TempDir();
    logserver::CLogStorage storage;
    ASSERT_TRUE(storage.SetDirectory(strDir));

    ASSERT_TRUE(storage.Write(MakeRecord("info", "example", "hello storage")));
    ASSERT_TRUE(storage.Write(MakeRecord("info", "servertemplate", "hello servertemplate")));
    ASSERT_EQ(storage.FileCount(), static_cast<size_t>(2));

    std::string strExample = ReadFile(strDir + "/example.log");
    ASSERT_TRUE(strExample.find("[INFO]") != std::string::npos);
    ASSERT_TRUE(strExample.find("hello storage") != std::string::npos);

    std::string strServerTemplate = ReadFile(strDir + "/servertemplate.log");
    ASSERT_TRUE(strServerTemplate.find("hello servertemplate") != std::string::npos);

    std::remove((strDir + "/example.log").c_str());
    std::remove((strDir + "/servertemplate.log").c_str());
    std::remove(strDir.c_str());
}

/// @brief 存储：来源清理（防路径穿越）。
TEST(LogStorage_SanitizeSource)
{
    std::string strDir = TempDir();
    logserver::CLogStorage storage;
    ASSERT_TRUE(storage.SetDirectory(strDir));

    // 路径分隔符与冒号等非法字符应被清理，仅保留字母数字 _ - .
    ASSERT_TRUE(storage.Write(MakeRecord("warn", "a/b\\c:evil", "sanitized")));

    std::string strFile = ReadFile(strDir + "/abcevil.log");
    ASSERT_TRUE(strFile.find("sanitized") != std::string::npos);
    ASSERT_TRUE(strFile.find("[WARN]") != std::string::npos);

    std::remove((strDir + "/abcevil.log").c_str());
    std::remove(strDir.c_str());
}
