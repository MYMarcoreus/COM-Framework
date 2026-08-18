#pragma once

#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "Thread/ThreadPool.h"

namespace common {

class CAsyncExecutor;

namespace detail {

/// @brief 任务共享状态：结果/异常 + 续接列表。
///
/// 续接在状态就绪时触发，成功与异常分开处理，保证链式传播。
template <typename T>
class CTaskState
{
public:
    /// @brief 续接回调：value 为 nullptr 表示异常。
    using Continuation = std::function<void(const T* value, const std::exception_ptr& eptr)>;

    CTaskState() : m_bReady(false) {}

    // 提供 future（供 Get 阻塞等待）。
    std::future<T> GetFuture() { return m_promise.get_future(); }

    // 成功完成并触发续接。
    void CompleteSuccess(const T& value)
    {
        std::vector<Continuation> vecCbs;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bReady)
            {
                return;
            }
            m_bReady = true;
            m_pValue = std::make_shared<T>(value);
            vecCbs.swap(m_vecContinuations);
        }
        m_promise.set_value(value);
        for (size_t i = 0; i < vecCbs.size(); ++i)
        {
            if (vecCbs[i])
            {
                vecCbs[i](m_pValue.get(), nullptr);
            }
        }
    }

    // 异常完成并触发续接。
    void CompleteFailure(const std::exception_ptr& eptr)
    {
        std::vector<Continuation> vecCbs;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bReady)
            {
                return;
            }
            m_bReady = true;
            m_exception = eptr;
            vecCbs.swap(m_vecContinuations);
        }
        m_promise.set_exception(eptr);
        for (size_t i = 0; i < vecCbs.size(); ++i)
        {
            if (vecCbs[i])
            {
                vecCbs[i](nullptr, eptr);
            }
        }
    }

    // 注册续接；若已就绪则立即触发。
    void AddContinuation(const Continuation& fnCallback)
    {
        const T* pValue = nullptr;
        std::exception_ptr eptr;
        bool bFireNow = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bReady)
            {
                bFireNow = true;
                pValue = m_pValue.get();
                eptr = m_exception;
            }
            else
            {
                m_vecContinuations.push_back(fnCallback);
            }
        }
        if (bFireNow && fnCallback)
        {
            fnCallback(pValue, eptr);
        }
    }

private:
    std::promise<T> m_promise;
    mutable std::mutex m_mutex;
    std::vector<Continuation> m_vecContinuations;
    bool m_bReady;
    std::shared_ptr<T> m_pValue;
    std::exception_ptr m_exception;
};

} // namespace detail

/// @brief 异步任务，支持链式调用（Then）。
///
/// 由 CAsyncExecutor::Submit / CTask::FromResult 创建，
/// 通过 Then 串联后续步骤，Get 阻塞获取最终结果。
template <typename T>
class CTask
{
public:
    // 创建空任务。
    CTask() : m_pExecutor(nullptr), m_pState(std::make_shared<detail::CTaskState<T> >()) {}

    // 从已就绪的值创建任务（可启动链式调用）。
    static CTask<T> FromResult(const T& value)
    {
        CTask<T> task;
        task.m_pState->CompleteSuccess(value);
        return task;
    }

    // 链式续接：本任务成功后，在执行器上运行 f(value)。
    template <typename F>
    CTask<typename std::result_of<F(T)>::type> Then(F f);

    // 阻塞获取结果；失败时抛出异常。
    T Get() const { return m_pState->GetFuture().get(); }

    // 注册成功回调（fire-and-forget）。
    void OnSuccess(const std::function<void(const T&)>& fnCallback);

    // 注册失败回调（fire-and-forget）。
    void OnFailure(const std::function<void(const std::exception_ptr&)>& fnCallback);

private:
    friend class CAsyncExecutor;
    template <typename U> friend class CTask;

    CAsyncExecutor* m_pExecutor;
    std::shared_ptr<detail::CTaskState<T> > m_pState;
};

/// @brief 异步执行器。
///
/// 基于线程池执行任务，支持链式调用（Submit → Then → Get）。
///
/// @note executor 必须存续到所有任务完成后再销毁。
class CAsyncExecutor
{
public:
    // 创建执行器（指定工作线程数）。
    explicit CAsyncExecutor(size_t nThreadCount = 1);

    ~CAsyncExecutor();

    // 启动工作线程。
    bool Start();

    // 提交任务并返回 CTask。
    template <typename F>
    CTask<typename std::result_of<F()>::type> Submit(F f);

    // 提交无返回值任务。
    bool Post(const std::function<void()>& fnTask);

    // 停止并等待任务完成。
    void Stop();

    // 是否正在运行。
    bool IsRunning() const;

private:
    std::unique_ptr<CThreadPool> m_pPool;
    size_t m_nThreadCount;
};

// ================= 模板实现 ================

template <typename T>
template <typename F>
CTask<typename std::result_of<F(T)>::type> CTask<T>::Then(F f)
{
    using R = typename std::result_of<F(T)>::type;
    CTask<R> taskNext;
    taskNext.m_pExecutor = m_pExecutor;
    std::shared_ptr<detail::CTaskState<R> > pNextState = taskNext.m_pState;
    CAsyncExecutor* pExecutor = m_pExecutor;

    m_pState->AddContinuation(
        [pExecutor, pNextState, f](const T* pValue, const std::exception_ptr& eptr)
        {
            // ① 上游失败：异常传播给下游
            if (eptr)
            {
                pNextState->CompleteFailure(eptr);
                return;
            }
            // ② 拷贝值，供异步续接安全使用
            T valueCopied = *pValue;
            std::function<void()> fnRun = [pNextState, f, valueCopied]()
            {
                try
                {
                    pNextState->CompleteSuccess(f(valueCopied));
                }
                catch (...)
                {
                    pNextState->CompleteFailure(std::current_exception());
                }
            };
            // ③ 在 pExecutor 上执行；无执行器时内联执行
            if (pExecutor != nullptr)
            {
                if (!pExecutor->Post(fnRun))
                {
                    pNextState->CompleteFailure(std::make_exception_ptr(
                        std::runtime_error("CAsyncExecutor 已停止")));
                }
            }
            else
            {
                fnRun();
            }
        });
    return taskNext;
}

template <typename T>
void CTask<T>::OnSuccess(const std::function<void(const T&)>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const T* pValue, const std::exception_ptr& eptr)
        {
            if (!eptr && fnCallback)
            {
                fnCallback(*pValue);
            }
        });
}

template <typename T>
void CTask<T>::OnFailure(const std::function<void(const std::exception_ptr&)>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const T*, const std::exception_ptr& eptr)
        {
            if (eptr && fnCallback)
            {
                fnCallback(eptr);
            }
        });
}

template <typename F>
CTask<typename std::result_of<F()>::type> CAsyncExecutor::Submit(F f)
{
    using R = typename std::result_of<F()>::type;
    CTask<R> task;
    task.m_pExecutor = this;
    std::shared_ptr<detail::CTaskState<R> > pState = task.m_pState;
    std::function<void()> fnRun = [pState, f]()
    {
        try
        {
            pState->CompleteSuccess(f());
        }
        catch (...)
        {
            pState->CompleteFailure(std::current_exception());
        }
    };
    if (!m_pPool || !m_pPool->Submit(fnRun))
    {
        pState->CompleteFailure(
            std::make_exception_ptr(std::runtime_error("CAsyncExecutor 未启动")));
    }
    return task;
}

} // namespace common
