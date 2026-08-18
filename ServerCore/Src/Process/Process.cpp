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
bool CProcess::Daemonize()
{
    // ① 第一次 fork
    pid_t nPid = ::fork();
    if (nPid < 0)
    {
        return false;
    }
    if (nPid > 0)
    {
        ::exit(0); // 父进程退出
    }

    // ② 新会话，脱离控制终端
    if (::setsid() < 0)
    {
        return false;
    }

    // ③ 第二次 fork，防止重新获取控制终端
    nPid = ::fork();
    if (nPid < 0)
    {
        return false;
    }
    if (nPid > 0)
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
    int nDevnull = ::open("/dev/null", O_RDWR);
    if (nDevnull >= 0)
    {
        static_cast<void>(::dup2(nDevnull, STDIN_FILENO));
        static_cast<void>(::dup2(nDevnull, STDOUT_FILENO));
        static_cast<void>(::dup2(nDevnull, STDERR_FILENO));
        if (nDevnull > 2)
        {
            static_cast<void>(::close(nDevnull));
        }
    }
    return true;
}

/// @brief 将 pid 写入文件。
///
/// @return 写入成功返回 true。
bool CProcess::WritePidFile(const std::string& strPath, pid_t nPid)
{
    int nFd = ::open(strPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                     static_cast<mode_t>(0644));
    if (nFd < 0)
    {
        return false;
    }
    char szBuffer[32];
    int nLen = std::snprintf(szBuffer, sizeof(szBuffer), "%ld\n",
                             static_cast<long>(nPid));
    bool bOk = (nLen > 0) &&
               (::write(nFd, szBuffer, static_cast<size_t>(nLen)) == nLen);
    static_cast<void>(::close(nFd));
    return bOk;
}

/// @brief 从文件读取 pid。
///
/// @return pid；文件不存在或格式非法返回 -1。
pid_t CProcess::ReadPidFile(const std::string& strPath)
{
    FILE* pFile = ::fopen(strPath.c_str(), "r");
    if (pFile == nullptr)
    {
        return -1;
    }
    long nValue = -1;
    if (std::fscanf(pFile, "%ld", &nValue) != 1)
    {
        static_cast<void>(::fclose(pFile));
        return -1;
    }
    static_cast<void>(::fclose(pFile));
    return static_cast<pid_t>(nValue);
}

/// @brief 检查进程是否存活。
bool CProcess::IsAlive(pid_t nPid)
{
    if (nPid <= 0)
    {
        return false;
    }
    return ::kill(nPid, 0) == 0;
}

/// @brief 删除 pid 文件。
bool CProcess::RemovePidFile(const std::string& strPath)
{
    return ::unlink(strPath.c_str()) == 0;
}

/// @brief 创建 pid 文件并写入当前进程 pid。
CPidFile::CPidFile(const std::string& strPath)
    : m_strPath(strPath), m_pid(::getpid()), m_bValid(false)
{
    m_bValid = CProcess::WritePidFile(m_strPath, m_pid);
}

/// @brief 销毁时删除 pid 文件。
CPidFile::~CPidFile()
{
    CProcess::RemovePidFile(m_strPath);
}

/// @brief 是否成功写入。
bool CPidFile::IsValid() const
{
    return m_bValid;
}

/// @brief 当前进程 pid。
pid_t CPidFile::Pid() const
{
    return m_pid;
}

} // namespace sc
