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

/// @brief 执行器句柄（生命周期加固核心）。
///
/// promise / 协程持有本句柄：执行器析构后线程池对象仍存活（已投递任务跑完），
/// 新投递被 m_bStopped 拒绝并转为拒绝结果。
struct CExecutorHandle
{
    std::shared_ptr<common::thread::CThreadPool> m_pPool;  ///< 工作线程池。
    std::atomic<bool> m_bStopped;                          ///< 是否已停止（拒绝新投递）。

    CExecutorHandle() : m_bStopped(false) {}
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

}  // namespace detail

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
    const std::shared_ptr<detail::CExecutorHandle>& Handle() const { return m_pHandle; }

   private:
    std::shared_ptr<detail::CExecutorHandle> m_pHandle;  ///< 执行器句柄（promise / 协程共享）。
    size_t m_nThreadCount;                               ///< 工作线程数。
};

}  // namespace async
}  // namespace common
