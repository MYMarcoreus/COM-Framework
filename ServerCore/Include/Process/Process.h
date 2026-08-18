#pragma once

#include <string>

#include <sys/types.h>

namespace sc {

/// @brief 进程管理工具。
///
/// 提供守护进程化与 pid 文件读写等服务器进程级基础设施（Linux/POSIX）。
class Process
{
public:
    // 守护进程化：fork 分离会话与进程组，父进程退出，子进程转为后台运行。
    static bool Daemonize();

    // 将 pid 写入文件（含换行）。
    static bool WritePidFile(const std::string& path, pid_t pid);

    // 从文件读取 pid；失败返回 -1。
    static pid_t ReadPidFile(const std::string& path);

    // 检查进程是否存活。
    static bool IsAlive(pid_t pid);

    // 删除 pid 文件。
    static bool RemovePidFile(const std::string& path);
};

/// @brief pid 文件（RAII）。
///
/// 构造时写入当前进程 pid，析构时删除文件，避免服务退出后残留 pid 文件。
class PidFile
{
public:
    explicit PidFile(const std::string& path);

    ~PidFile();

    // 是否成功写入 pid 文件。
    bool IsValid() const;

    // 当前进程 pid。
    pid_t Pid() const;

private:
    PidFile(const PidFile&) = delete;
    PidFile& operator=(const PidFile&) = delete;

    std::string path_;
    pid_t pid_;
    bool valid_;
};

} // namespace sc
