#include "Application/MyApplication.h"

#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

namespace sc {

namespace
{
/// 全局停止请求标记，信号处理程序写入。
std::atomic<bool> g_stopRequested(false);

/// 当前应用程序实例，信号处理程序使用。
MyApplication* g_instance = nullptr;
} // namespace

/// @brief 创建服务器应用程序。
MyApplication::MyApplication() : running_(false)
{
}

/// @brief 销毁服务器应用程序。
MyApplication::~MyApplication()
{
}

/// @brief 初始化服务器应用程序。
///
/// 注册基础组件并调用派生类的初始化钩子。
///
/// @return true 初始化成功；false 初始化失败。
bool MyApplication::Initialize()
{
    if (!RegisterComponents())
    {
        return false;
    }
    return OnInitialize();
}

/// @brief 启动服务器应用程序。
///
/// @return true 启动成功；false 启动失败。
bool MyApplication::Start()
{
    return OnStart();
}

/// @brief 运行服务器应用程序主循环。
///
/// 安装 SIGINT / SIGTERM 信号处理程序，进入主循环，直到收到停止信号。
///
/// @return 主循环退出码，0 表示正常退出。
int MyApplication::Run()
{
    running_.store(true);
    g_stopRequested.store(false);
    g_instance = this;

    //================ Signal Setup ================

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &MyApplication::HandleSignal;
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
void MyApplication::Stop()
{
    running_.store(false);
}

/// @brief 关闭服务器应用程序并释放资源。
void MyApplication::Shutdown()
{
    OnShutdown();
    componentManager_.Clear();
}

/// @brief 获取组件管理器。
ComponentManager& MyApplication::GetComponentManager()
{
    return componentManager_;
}

/// @brief 注册基础组件。
///
/// 基类不注册任何组件，派生类根据需要重写。
bool MyApplication::RegisterComponents()
{
    return true;
}

/// @brief 初始化完成钩子。
bool MyApplication::OnInitialize()
{
    return true;
}

/// @brief 启动完成钩子。
bool MyApplication::OnStart()
{
    return true;
}

/// @brief 主循环钩子。
///
/// 默认实现：等待停止信号，循环检查运行标记。
int MyApplication::OnRun()
{
    while (running_.load() && !g_stopRequested.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
}

/// @brief 停止钩子。
void MyApplication::OnStop()
{
}

/// @brief 关闭钩子。
void MyApplication::OnShutdown()
{
}

/// @brief 信号处理入口。
///
/// @param signo 信号编号。
void MyApplication::HandleSignal(int signo)
{
    (void)signo;
    g_stopRequested.store(true);
    if (g_instance != nullptr)
    {
        g_instance->running_.store(false);
    }
}

} // namespace sc
