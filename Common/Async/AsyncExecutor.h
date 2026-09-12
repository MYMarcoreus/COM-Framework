#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

#include "Async/PromiseResult.h"
#include "Async/PromiseTypes.h"
#include "Async/SourceLoc.h"
#include "Thread/ThreadPool.h"

// ====================================================================
// 异步执行器（调度层）
//
// 职责：持有工作线程池，提供「投递执行」与「起 promise / 起协程」的入口。
// 不做编排（编排见 Promise.h）、不做顺序化（见 Coroutine.h）。
//
// 用法：
//   common::async::CAsyncExecutor exec(4);
//   exec.Start();
//   exec.Post([]() { /* 无返回值任务 */ });
//   auto p = exec.NewPromise(spCtx, StepLoad).Then(StepSave);   // 起 promise
//   exec.Stop();
//
// 生命周期：执行器析构会停止线程池并等待已投递任务完成；promise / 协程通过
// 共享句柄引用线程池，执行器析构后已起动的 promise 仍安全跑完（新投递以
// kStopped 被拒绝）。
// ====================================================================

namespace common {
namespace async {

template <typename TContext>
class CPromise;  // 前置声明（NewPromise 返回 promise 句柄）。

namespace detail {

/// @brief 执行器侧诊断文案（集中一处：测试断言常量，而不是去匹配子串）。
constexpr const char* kDiagPostThrow = "exec.Post() 投递的任务抛出了异常（已兜住，未终止进程）";
constexpr const char* kDiagPostEmpty = "exec.Post(): 任务为空（未提交）";

/// @brief 执行器句柄（生命周期加固核心）。
///
/// promise / 协程持有本句柄：执行器析构后线程池对象仍存活（已投递任务跑完），
/// 新投递被 m_bStopped 拒绝并转为拒绝结果。
struct CExecutorHandle
{
    std::shared_ptr<common::thread::CThreadPool> m_pPool;  ///< 工作线程池。
    std::atomic<bool> m_bStopped;                          ///< 是否已停止（拒绝新投递）。

    CExecutorHandle() : m_bStopped(false)
    {}
};

/// @brief 向执行器句柄投递任务（句柄不可用时返回 false，不抛异常）。
///
/// @param pHandle 执行器句柄。
/// @param fnTask 任务函数（移动投递）。
/// @return true 投递成功；false 句柄不可用（空 / 已停止 / 线程池拒绝）。
inline bool PostToHandle(const std::shared_ptr<CExecutorHandle>& pHandle, std::function<void()> fnTask)
{
    if (pHandle == nullptr || pHandle->m_pPool == nullptr || pHandle->m_bStopped)
    {
        return false;
    }
    return pHandle->m_pPool->Submit(std::move(fnTask));
}

/// @brief 当前线程是否是某执行器的工作线程（线程亲和判定）。
///
/// 层处理器只在「已经在本链执行器线程上」时就地内联，否则投递回本链执行器，
/// 从而保证「每一层都跑在它所属链的执行器线程上」。
///
/// @param pHandle 执行器句柄。
/// @return true 当前线程是该执行器的工作线程。
inline bool IsInExecutorThread(const std::shared_ptr<CExecutorHandle>& pHandle)
{
    return pHandle != nullptr && common::thread::CThreadPool::IsInPoolThread(pHandle->m_pPool.get());
}

}  // namespace detail

/// @brief 诊断处理器：接收一句「框架检测到的用法问题」描述（不带换行）。
///
/// 用途：把「不致命但肯定是 bug」的用法报告出来（通知里抛异常、在层内阻塞等待
/// 未落定的层、在无效 promise 上挂层等）。应用可接日志 / 指标；测试可接断言。
using DiagnosticHandler = std::function<void(const char* strWhat)>;

/// @brief 设置诊断处理器（线程安全）。
///
/// @param fnHandler 处理器；传 `nullptr` 恢复默认（debug 构建打印到 stderr，发布构建忽略）；
///                  想完全关闭传一个空 lambda。
void SetDiagnosticHandler(const DiagnosticHandler& fnHandler);

/// @brief 报告一次诊断（框架内部用；未设处理器时按默认策略处理，不会抛异常）。
///
/// @param strWhat 问题描述（静态字符串，生命周期无要求）。
void ReportDiagnostic(const char* strWhat);

/// @brief 异步执行器：工作线程池 + 投递入口。
///
/// 非模板类；起 promise 通过模板成员 NewPromise 完成（上下文类型由参数推导）。
class CAsyncExecutor
{
public:
    /// @brief 创建执行器。
    ///
    /// @param nThreadCount 工作线程数（默认 1）。
    explicit CAsyncExecutor(size_t nThreadCount = 1);

    /// @brief 不可拷贝（拷贝会共享线程池，Stop 相互影响）。
    CAsyncExecutor(const CAsyncExecutor&) = delete;
    CAsyncExecutor& operator=(const CAsyncExecutor&) = delete;

    /// @brief 销毁执行器（停止线程池并等待已投递任务完成）。
    ~CAsyncExecutor();

    /// @brief 启动工作线程。
    ///
    /// @return true 启动成功；false 已启动或线程数为 0。
    bool Start();

    /// @brief 停止并等待任务完成（优雅关闭）。
    void Stop();

    /// @brief 是否正在运行。
    bool IsRunning() const;

    /// @brief 是否已停止（停止后拒绝新投递）。
    bool IsStopped() const;

    /// @brief 线程池是否空闲（无排队任务；协程内联续接判断用）。
    bool IsIdle() const;

    /// @brief 当前线程是否本执行器的工作线程（线程亲和判定）。
    ///
    /// 层处理器与协程续跑只在本执行器线程上就地执行，否则投递回本执行器。
    ///
    /// @return true 当前线程是本执行器的工作线程。
    bool IsInExecutorThread() const
    {
        return detail::IsInExecutorThread(m_pHandle);
    }

    /// @brief 投递无返回值任务（fire-and-forget）。
    ///
    /// @param fnTask 任务函数（按值接收，移动投递避免拷贝）。
    /// @return true 提交成功；false 执行器未启动 / 已停止。
    bool Post(std::function<void()> fnTask);

    /// @brief 起 promise（等价 JS `new Promise(executor)`）：创建 promise 并投递首层。
    ///
    /// 数据经共享上下文（spContext）在层间共享；层与层之间只传兑现 / 拒绝。
    /// 首层在调用返回后异步执行，不在调用线程上执行。
    ///
    /// 需要「由外部回调兑现 / 拒绝」（把其他模块 / 回调式异步接进来）时用
    /// `CPromise<TContext>::New(exec, spCtx, executor, loc)`（见 Promise.h）。
    ///
    /// @tparam TContext 上下文类型（由 spContext 推导）。
    /// @param spContext promise 的共享上下文（所有层共用同一实例）。
    /// @param fnHandler 首层处理器（固定签名）。
    /// @param loc 注册点源码位置（可选，建议传 ASYNC_LOC）。
    /// @return 指向首层的 promise 句柄；执行器不可用时首层立即被拒绝（kStopped）。
    template <typename TContext>
    CPromise<TContext> NewPromise(const std::shared_ptr<TContext>& spContext,
                                  typename CPromise<TContext>::ThenHandler fnHandler,
                                  const CSourceLoc& loc = CSourceLoc());

    /// @brief 建一条「延迟启动」的 promise 链（先挂完所有层，再 `Start()`）。
    ///
    /// 与 `NewPromise` 的差别：**追加层只登记，不投递**；`Start()`（或首次 `Await()`）后
    /// 首层才投递执行。好处：
    ///  - 构链期间不跑任何业务代码（可放心初始化上下文 / 挂完所有层）；
    ///  - 所有层都在首层开跑前登记完毕 → 跨模块续接不再出现“补登记”的时序差异。
    ///
    /// 用法：
    /// @code
    /// auto p = exec.BuildPromise(spCtx, ASYNC_LOC).Then(StepA).ThenPromise(fnCallOther).Then(StepB);
    /// p.Start();            // 此刻才开始跑（不调 Start 直接 Await 也行，会自动启动）
    /// p.Await();
    /// @endcode
    ///
    /// @tparam TContext 上下文类型（由 spContext 推导）。
    /// @param spContext 共享上下文（所有层共用）。
    /// @return 未启动的链句柄。
    template <typename TContext>
    CPromise<TContext> BuildPromise(const std::shared_ptr<TContext>& spContext);

    /// @brief 创建并启动协程（投递首次 Resume；返回 shared_ptr 管理生命周期）。
    ///
    /// 协程类型须继承 common::async::CCoroutine<TContext> 并实现 Run()。
    /// 定义见 "Async/Coroutine.h"。
    ///
    /// @tparam TCoroutine 协程类型。
    /// @tparam TArgs 协程构造参数类型。
    /// @param args 转发给 TCoroutine 构造函数的参数。
    /// @return 协程对象；调用方须持有直到完成（Await() 取结果），勿丢弃。
    template <typename TCoroutine, typename... TArgs>
    std::shared_ptr<TCoroutine> CoStart(TArgs&&... args);

    /// @brief 内部：执行器句柄（promise / 协程持有，生命周期加固用）。
    const std::shared_ptr<detail::CExecutorHandle>& Handle() const
    {
        return m_pHandle;
    }

private:
    std::shared_ptr<detail::CExecutorHandle> m_pHandle;  ///< 执行器句柄（promise / 协程共享）。
    size_t m_nThreadCount;                               ///< 工作线程数。
};

}  // namespace async
}  // namespace common
