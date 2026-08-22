#include "Module/DemoAsyncModule.h"

#include <stdexcept>
#include <string>

#include "Async/AsyncExecutor.h"
#include "Infra/GuardedTimer.h"
#include "Log/Logger.h"
#include "Module/ResolveContext.h"

namespace demo {

/// @brief 创建异步演示模块。
///
/// @param intervalMs 演示周期（毫秒，小于 100 按 100 处理）。
CDemoAsyncModule::CDemoAsyncModule(std::int64_t intervalMs)
    : sc::CModule("async-demo"), m_nIntervalMs(intervalMs), m_tTimerId(common::kInvalidTimerId)
{
    // 依赖定时器接口模块：周期触发演示。
    AddDependency(sc::IID_ITimer());
    if (m_nIntervalMs < 100)
    {
        m_nIntervalMs = 100;
    }
}

/// @brief 销毁异步演示模块。
CDemoAsyncModule::~CDemoAsyncModule()
{
    Stop();
}

/// @brief 解析 ITimer 接口。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true 定时器接口就绪；false 缺失。
bool CDemoAsyncModule::Initialize(const sc::CResolveContext& ctx)
{
    m_pTimer.Reset(ctx.Resolve<sc::ITimer>());
    return m_pTimer != nullptr;
}

/// @brief 启动自建执行器并注册周期演示定时器。
///
/// 自建 common::CAsyncExecutor：CTask 链式调用需要具体类型
/// （IAsyncExecutor 接口只暴露 Post，模板无法进虚函数表）。
///
/// @return true 启动成功；false 执行器或定时器接口缺失。
bool CDemoAsyncModule::Start()
{
    if (m_pTimer == nullptr)
    {
        return false;
    }
    m_pExecutor.reset(new common::CAsyncExecutor(2));
    if (!m_pExecutor->Start())
    {
        return false;
    }
    // 周期演示：弱引用守卫，模块停止/销毁后回调自动跳过。
    m_tTimerId = sc::AddGuardedPeriodicTimer(m_pTimer.Get(), m_nIntervalMs,
        WeakSelf<CDemoAsyncModule>(),
        [](const sc::ScopedInterfacePtr<CDemoAsyncModule>& sp)
        {
            sp->RunDemo();
        });
    return true;
}

/// @brief 取消定时器并停止执行器。
///
/// CAsyncExecutor::Stop 等待已提交任务完成（优雅关闭）。
void CDemoAsyncModule::Stop()
{
    if (m_tTimerId != common::kInvalidTimerId)
    {
        if (m_pTimer != nullptr)
        {
            m_pTimer->Cancel(m_tTimerId);
        }
        m_tTimerId = common::kInvalidTimerId;
    }
    if (m_pExecutor != nullptr)
    {
        m_pExecutor->Stop();
        m_pExecutor.reset();
    }
}

/// @brief 停止并释放接口引用。
void CDemoAsyncModule::Shutdown()
{
    Stop();
    m_pTimer.Reset();
}

/// @brief 执行一次完整的异步能力演示。
void CDemoAsyncModule::RunDemo()
{
    if (m_pExecutor == nullptr)
    {
        return;
    }

    // ① 链式调用：Submit → Then → Then（多阶段流水线 3 → 6 → 7）
    common::CTask<int> task = m_pExecutor->Submit([]() { return 3; })
        .Then([](int n) { return n * 2; })
        .Then([](int n) { return n + 1; });

    // ② 多回调 fan-out：同一任务多个消费者（全部触发）
    task.OnSuccess([](int n)
    {
        common::CLogger::Instance().Info("[AsyncDemo] 链式结果=" + std::to_string(n));
    });
    task.OnSuccess([](int n)
    {
        common::CLogger::Instance().Info("[AsyncDemo] fan-out 第二个消费者收到=" + std::to_string(n));
    });

    // ③ 异常沿链传播：OnFailure 处理上游异常
    common::CTask<int> failTask = m_pExecutor->Submit([]() -> int
    {
        throw std::runtime_error("演示用异常");
    });
    failTask.OnFailure([](const std::exception_ptr& eptr)
    {
        try
        {
            std::rethrow_exception(eptr);
        }
        catch (const std::exception& e)
        {
            common::CLogger::Instance().Warn(std::string("[AsyncDemo] 异常沿链传播: ") + e.what());
        }
    });

    // ④ 阻塞获取（演示 Get）
    int nResult = task.Get();
    common::CLogger::Instance().Info("[AsyncDemo] Get 阻塞获取=" + std::to_string(nResult));
}

} // namespace demo
