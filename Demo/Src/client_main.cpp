#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "Protocol/DemoProtocol.h"

namespace
{
// 读取指定字节数。
bool ReadFull(int fd, char* buffer, size_t len);

// 发送完整数据。
bool WriteFull(int fd, const char* data, size_t len);

// 发送请求并读取响应。
void DoRequest(int fd, const std::string& request, const char* label);
} // namespace

/// @brief Demo 测试客户端入口。
///
/// 连接 Demo 服务器，发送 PING 与 ECHO 请求并打印响应。
///
/// @param argv[1] 服务器端口（可选，默认 9000）。
int main(int argc, char* argv[])
{
    const char* host = "127.0.0.1";
    std::uint16_t port = 9000;
    if (argc > 1)
    {
        int value = std::atoi(argv[1]);
        if (value > 0 && value <= 65535)
        {
            port = static_cast<std::uint16_t>(value);
        }
    }

    // ① 创建连接
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        std::perror("socket");
        return -1;
    }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr(host);
    addr.sin_port = htons(port);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::perror("connect");
        ::close(fd);
        return -1;
    }
    std::printf("已连接 %s:%u\n", host, static_cast<unsigned>(port));

    // ② 发送 PING，期望 PONG
    std::string ping = demo::DemoProtocol::BuildPing();
    DoRequest(fd, ping, "PING");

    // ③ 发送 ECHO，期望相同负载
    std::string echo = demo::DemoProtocol::BuildPacket(demo::kCmdEcho, "Hello ServerCore");
    DoRequest(fd, echo, "ECHO");

    ::close(fd);
    std::printf("客户端退出\n");
    return 0;
}

namespace
{

/// @brief 读取指定字节数。
bool ReadFull(int fd, char* buffer, size_t len)
{
    size_t received = 0;
    while (received < len)
    {
        ssize_t n = ::recv(fd, buffer + received, len - received, 0);
        if (n <= 0)
        {
            return false;
        }
        received += static_cast<size_t>(n);
    }
    return true;
}

/// @brief 发送完整数据。
bool WriteFull(int fd, const char* data, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

/// @brief 发送请求并读取响应。
void DoRequest(int fd, const std::string& request, const char* label)
{
    if (!WriteFull(fd, request.data(), request.size()))
    {
        std::printf("[%s] 发送失败\n", label);
        return;
    }
    // 读取头部
    char header[4];
    if (!ReadFull(fd, header, sizeof(header)))
    {
        std::printf("[%s] 读取响应失败\n", label);
        return;
    }
    std::uint32_t len = 0;
    std::memcpy(&len, header, sizeof(len));
    len = ntohl(len);
    if (len < 1 || len > demo::DemoProtocol::kMaxPacketSize)
    {
        std::printf("[%s] 非法响应长度: %u\n", label, static_cast<unsigned>(len));
        return;
    }
    std::string body(len, '\0');
    if (!ReadFull(fd, &body[0], len))
    {
        std::printf("[%s] 读取响应体失败\n", label);
        return;
    }
    std::uint8_t command = static_cast<std::uint8_t>(body[0]);
    std::string payload = body.substr(1);
    std::printf("[%s] 命令=%u 负载=\"%s\"\n", label, static_cast<unsigned>(command), payload.c_str());
}

} // namespace
