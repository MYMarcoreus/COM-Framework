#include "Process/Process.h"

#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sc {

/// @brief 守护进程化。
///
/// 标准守护化流程：fork → setsid → 二次 fork → chdir("/") → umask(0) →
/// 标准流重定向到 /dev/null。
///
/// @note 仅在子进程中返回 true；父进程在 fork 后直接退出。
bool Process::Daemonize()
{
    // ① 第一次 fork
    pid_t pid = ::fork();
    if (pid < 0)
    {
        return false;
    }
    if (pid > 0)
    {
        ::exit(0); // 父进程退出
    }

    // ② 新会话，脱离控制终端
    if (::setsid() < 0)
    {
        return false;
    }

    // ③ 第二次 fork，防止重新获取控制终端
    pid = ::fork();
    if (pid < 0)
    {
        return false;
    }
    if (pid > 0)
    {
        ::exit(0);
    }

    // ④ 工作目录与文件权限掩码
    if (::chdir("/") < 0)
    {
        return false;
    }
    ::umask(0);

    // ⑤ 标准流重定向到 /dev/null
    int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0)
    {
        static_cast<void>(::dup2(devnull, STDIN_FILENO));
        static_cast<void>(::dup2(devnull, STDOUT_FILENO));
        static_cast<void>(::dup2(devnull, STDERR_FILENO));
        if (devnull > 2)
        {
            static_cast<void>(::close(devnull));
        }
    }
    return true;
}

/// @brief 将 pid 写入文件。
///
/// @return 写入成功返回 true。
bool Process::WritePidFile(const std::string& path, pid_t pid)
{
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                    static_cast<mode_t>(0644));
    if (fd < 0)
    {
        return false;
    }
    char buffer[32];
    int len = std::snprintf(buffer, sizeof(buffer), "%ld\n",
                            static_cast<long>(pid));
    bool ok = (len > 0) &&
              (::write(fd, buffer, static_cast<size_t>(len)) == len);
    static_cast<void>(::close(fd));
    return ok;
}

/// @brief 从文件读取 pid。
///
/// @return pid；文件不存在或格式非法返回 -1。
pid_t Process::ReadPidFile(const std::string& path)
{
    FILE* file = ::fopen(path.c_str(), "r");
    if (file == nullptr)
    {
        return -1;
    }
    long value = -1;
    if (std::fscanf(file, "%ld", &value) != 1)
    {
        static_cast<void>(::fclose(file));
        return -1;
    }
    static_cast<void>(::fclose(file));
    return static_cast<pid_t>(value);
}

/// @brief 检查进程是否存活。
bool Process::IsAlive(pid_t pid)
{
    if (pid <= 0)
    {
        return false;
    }
    return ::kill(pid, 0) == 0;
}

/// @brief 删除 pid 文件。
bool Process::RemovePidFile(const std::string& path)
{
    return ::unlink(path.c_str()) == 0;
}

/// @brief 创建 pid 文件并写入当前进程 pid。
PidFile::PidFile(const std::string& path)
    : path_(path), pid_(::getpid()), valid_(false)
{
    valid_ = Process::WritePidFile(path_, pid_);
}

/// @brief 销毁时删除 pid 文件。
PidFile::~PidFile()
{
    Process::RemovePidFile(path_);
}

/// @brief 是否成功写入。
bool PidFile::IsValid() const
{
    return valid_;
}

/// @brief 当前进程 pid。
pid_t PidFile::Pid() const
{
    return pid_;
}

} // namespace sc
