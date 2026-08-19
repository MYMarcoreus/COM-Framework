#include "Application/MyApplication.h"

#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

#include "Infra/IConfig.h"
#include "Infra/ILogger.h"

namespace sc {

namespace
{
/// 全局停止请求标记，信号处理程序写入。
std::atomic<bool> g_stopRequested(false);

/// 当前应用程序实例，信号处理程序使用。
CMyApplication* g_instance = nullptr;
} // namespace

/// @brief 创建服务器应用程序。
CMyApplication::CMyApplication() : m_bRunning(false), m_nShutdownTimeoutMs(0)
{
}

/// @brief 销毁服务器应用程序。
CMyApplication::~CMyApplication()
{
}

/// @brief 初始化服务器应用程序。
///
/// 注册基础模块与模块，统一初始化所有模块，再调用派生类初始化钩子。
///
/// @return true 初始化成功；false 初始化失败。
bool CMyApplication::Initialize()
{
    if (!RegisterModules())
    {
        return false;
    }
    if (!m_moduleManager.InitializeAll())
    {
        m_moduleManager.Clear();
        return false;
    }
    return OnInitialize();
}

/// @brief 启动服务器应用程序。
///
/// 统一启动所有模块，再调用派生类启动钩子。
///
/// @return true 启动成功；false 启动失败。
bool CMyApplication::Start()
{
    if (!m_moduleManager.StartAll())
    {
        return false;
    }
    return OnStart();
}

/// @brief 运行服务器应用程序主循环。
///
/// 安装 SIGINT / SIGTERM 信号处理程序，进入主循环，直到收到停止信号。
///
/// @return 主循环退出码，0 表示正常退出。
int CMyApplication::Run()
{
    m_bRunning.store(true);
    m_startTime = std::chrono::steady_clock::now();
    g_stopRequested.store(false);
    g_instance = this;

    //================ Signal Setup ================

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &CMyApplication::HandleSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int result = OnRun();

    // 恢复默认信号处理
    sa.sa_handler = SIG_DFL;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    g_instance = nullptr;
    return result;
}

/// @brief 停止服务器应用程序。
///
/// @note 可从信号处理程序调用，仅做原子写操作。
void CMyApplication::Stop()
{
    m_bRunning.store(false);
}

/// @brief 关闭服务器应用程序并释放资源。
///
/// 多阶段优雅关闭：先调用关闭钩子，再停止、关闭所有模块，
/// 然后带超时地停止、关闭模块，最后释放模块。
/// 若设置了关闭超时（SetShutdownTimeout），超时后跳过剩余阶段。
void CMyApplication::Shutdown()
{
    OnShutdown();
    m_moduleManager.StopAll();
    m_moduleManager.ShutdownAll();
    m_moduleManager.StopAllWithTimeout(m_nShutdownTimeoutMs);
    m_moduleManager.ShutdownAllWithTimeout(m_nShutdownTimeoutMs);
    m_moduleManager.Clear();
}

/// @brief 获取模块管理器。
CModuleManager& CMyApplication::GetModuleManager()
{
    return m_moduleManager;
}

/// @brief 设置配置文件路径。
void CMyApplication::SetConfigPath(const std::string& strPath)
{
    m_strConfigPath = strPath;
}

/// @brief 配置文件路径。
const std::string& CMyApplication::ConfigPath() const
{
    return m_strConfigPath;
}

/// @brief 是否正在运行。
bool CMyApplication::IsRunning() const
{
    return m_bRunning.load();
}

/// @brief 已运行秒数。
///
/// Run 启动后开始计时（含已结束的运行），供状态查询与退出时统计。
uint64_t CMyApplication::UptimeSeconds() const
{
    std::chrono::steady_clock::duration elapsed =
        std::chrono::steady_clock::now() - m_startTime;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
}

/// @brief 设置优雅关闭总超时。
///
/// @param nTimeoutMs 超时毫秒数；0 表示不限制。
void CMyApplication::SetShutdownTimeout(uint32_t nTimeoutMs)
{
    m_nShutdownTimeoutMs = nTimeoutMs;
}

/// @brief 当前优雅关闭超时（毫秒）。
uint32_t CMyApplication::ShutdownTimeout() const
{
    return m_nShutdownTimeoutMs;
}

/// @brief 注册应用程序需要的模块（默认装配）。
///
/// 默认注册配置模块与日志模块；若同接口标识模块已存在（派生类先注册）则跳过。
/// 派生类重写时可先调用本实现再注册业务模块。
///
/// @return true 全部成功。
bool CMyApplication::RegisterModules()
{
    if (m_moduleManager.GetModuleByIid(IID_IConfig()) == nullptr)
    {
        CConfigModule* pConfig = new CConfigModule();
        if (!m_strConfigPath.empty())
        {
            pConfig->LoadFile(m_strConfigPath);
        }
        if (!m_moduleManager.RegisterModule(IID_IConfig(), pConfig))
        {
            pConfig->Release();
            return false;
        }
        pConfig->Release(); // 管理器已持有引用
    }
    if (m_moduleManager.GetModuleByIid(IID_ILogger()) == nullptr)
    {
        CLoggerModule* pLogger = new CLoggerModule();
        if (!m_moduleManager.RegisterModule(IID_ILogger(), pLogger))
        {
            pLogger->Release();
            return false;
        }
        pLogger->Release(); // 管理器已持有引用
    }
    return true;
}

/// @brief 初始化完成钩子。
bool CMyApplication::OnInitialize()
{
    return true;
}

/// @brief 启动完成钩子。
bool CMyApplication::OnStart()
{
    return true;
}

/// @brief 主循环钩子。
///
/// 默认实现：等待停止信号，循环检查运行标记。
int CMyApplication::OnRun()
{
    while (m_bRunning.load() && !g_stopRequested.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
}

/// @brief 停止钩子。
void CMyApplication::OnStop()
{
}

/// @brief 关闭钩子。
void CMyApplication::OnShutdown()
{
}

/// @brief 信号处理入口。
///
/// @param signo 信号编号。
void CMyApplication::HandleSignal(int signo)
{
    (void)signo;
    g_stopRequested.store(true);
    if (g_instance != nullptr)
    {
        g_instance->m_bRunning.store(false);
    }
}

} // namespace sc
