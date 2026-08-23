#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <string>
#include <vector>

#include "Thread/ThreadPool.h"

// ====================================================================
// 无异常版异步框架（错误码 + CTaskResult<T>，风格类似 std::expected<T, Error>）
//
// 与 Common/Async/AsyncExecutor.h 的区别：
//  - 不使用 std::exception_ptr / 不抛异常；错误用错误码 + 消息传递；
//  - CTask::Get() 返回 CTaskResult<T>（不抛异常，需检查 Ok()/Failed()）；
//  - 任务内部抛出的异常被捕获并转为 kTaskFailed 错误码。
// ====================================================================

namespace common {
namespace nothrow {

class CAsyncExecutor; // 前向声明（CTask 持有其指针，定义在下方）

/// @brief 错误码（无异常的错误通道）。
enum ErrorCode
{
    kOk = 0,             // 成功
    kTaskFailed,         // 任务执行抛出异常（已转为错误）
    kExecutorNotStarted, // 执行器未启动
    kExecutorStopped     // 执行器已停止
};

/// @brief 错误信息：错误码 + 可读消息。
struct CError
{
    int nCode;         // 错误码（ErrorCode）
    std::string strMessage; // 错误描述

    CError() : nCode(kOk) {}
    CError(int nErrorCode, const std::string& strError)
        : nCode(nErrorCode), strMessage(strError) {}

    // 是否失败（非 kOk）。
    bool Failed() const { return nCode != kOk; }
};

/// @brief 任务结果（类似 std::expected<T, Error>）。
///
/// 成功携带值（Ok()），失败携带错误（Failed() / Error()）。
template <typename T>
class CTaskResult
{
public:
    // 成功结果。
    static CTaskResult<T> Success(const T& value)
    {
        CTaskResult<T> r;
        r.m_pValue.reset(new T(value));
        return r;
    }

    // 失败结果。
    static CTaskResult<T> Failure(int nCode, const std::string& strMessage)
    {
        CTaskResult<T> r;
        r.m_error = CError(nCode, strMessage);
        return r;
    }

    // 是否成功。
    bool Ok() const { return m_pValue != nullptr; }

    // 是否失败。
    bool Failed() const { return m_pValue == nullptr; }

    // 成功时的值（仅在 Ok() 时调用）。
    const T& Value() const { return *m_pValue; }

    // 失败时的错误（仅在 Failed() 时调用）。
    const CError& Error() const { return m_error; }

public:
    // 空结果（默认 kOk 无值）；通常用 Success / Failure 工厂创建。
    CTaskResult() : m_error(kOk, "") {}

private:
    std::shared_ptr<T> m_pValue;
    CError m_error;
};

/// @brief CTaskResult<void> 特化（成功无值）。
template <>
class CTaskResult<void>
{
public:
    // 成功结果。
    static CTaskResult<void> Success()
    {
        CTaskResult<void> r;
        r.m_bOk = true;
        return r;
    }

    // 失败结果。
    static CTaskResult<void> Failure(int nCode, const std::string& strMessage)
    {
        CTaskResult<void> r;
        r.m_bOk = false;
        r.m_error = CError(nCode, strMessage);
        return r;
    }

    // 是否成功。
    bool Ok() const { return m_bOk; }

    // 是否失败。
    bool Failed() const { return !m_bOk; }

    // 失败时的错误（仅在 Failed() 时调用）。
    const CError& Error() const { return m_error; }

public:
    // 空结果（默认 kOk 失败态）；通常用 Success / Failure 工厂创建。
    CTaskResult() : m_bOk(false), m_error(kOk, "") {}

private:
    bool m_bOk;
    CError m_error;
};

namespace detail {

/// @brief 任务共享状态（无异常版）：结果 + 续接列表 + 同步等待。
template <typename T>
class CTaskState
{
public:
    using Continuation = std::function<void(const CTaskResult<T>&)>;

    CTaskState() : m_bReady(false) {}

    // 完成并触发续接（锁外调用，防重入死锁）。
    void Complete(const CTaskResult<T>& result)
    {
        std::vector<Continuation> vecCbs;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bReady)
            {
                return;
            }
            m_bReady = true;
            m_result = result;
            vecCbs.swap(m_vecContinuations);
        }
        m_cv.notify_all();
        for (size_t i = 0; i < vecCbs.size(); ++i)
        {
            if (vecCbs[i])
            {
                vecCbs[i](result);
            }
        }
    }

    // 注册续接；已就绪则立即触发。
    void AddContinuation(const Continuation& fnCallback)
    {
        bool bFireNow = false;
        CTaskResult<T> result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_bReady)
            {
                bFireNow = true;
                result = m_result;
            }
            else
            {
                m_vecContinuations.push_back(fnCallback);
            }
        }
        if (bFireNow && fnCallback)
        {
            fnCallback(result);
        }
    }

    // 阻塞等待结果（返回 CTaskResult，不抛异常）。
    CTaskResult<T> Wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_bReady; });
        return m_result;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<Continuation> m_vecContinuations;
    bool m_bReady;
    CTaskResult<T> m_result;
};

// 完成成功：R 非 void（传值）；R 为 void 的特化（无值）。
template <typename R, typename F>
void CompleteSuccess(const std::shared_ptr<CTaskState<R> >& pState, F f)
{
    pState->Complete(CTaskResult<R>::Success(f()));
}

template <typename F>
void CompleteSuccess(const std::shared_ptr<CTaskState<void> >& pState, F f)
{
    f();
    pState->Complete(CTaskResult<void>::Success());
}

} // namespace detail

/// @brief 异步任务（无异常版），支持链式调用（Then）。
template <typename T>
class CTask
{
public:
    // 创建空任务。
    CTask() : m_pExecutor(nullptr), m_pState(std::make_shared<detail::CTaskState<T> >()) {}

    // 从已就绪结果创建任务。
    static CTask<T> FromResult(const CTaskResult<T>& result)
    {
        CTask<T> task;
        task.m_pState->Complete(result);
        return task;
    }

    // 链式续接：上游成功后在执行器上运行 f(value)，失败则传播错误。
    template <typename F>
    CTask<typename std::result_of<F(T)>::type> Then(F f);

    // 阻塞获取结果（不抛异常）。
    CTaskResult<T> Get() const { return m_pState->Wait(); }

    // 成功回调（fire-and-forget）。
    void OnSuccess(const std::function<void(const T&)>& fnCallback);

    // 失败回调（fire-and-forget）。
    void OnFailure(const std::function<void(const CError&)>& fnCallback);

private:
    friend class CAsyncExecutor;
    template <typename U> friend class CTask;

    CAsyncExecutor* m_pExecutor;
    std::shared_ptr<detail::CTaskState<T> > m_pState;
};

/// @brief 异步执行器（无异常版）。
class CAsyncExecutor
{
public:
    explicit CAsyncExecutor(size_t nThreadCount = 1);

    ~CAsyncExecutor();

    // 启动工作线程。
    bool Start();

    // 提交任务并返回 CTask（任务异常自动转为 kTaskFailed 错误）。
    template <typename F>
    CTask<typename std::result_of<F()>::type> Submit(F f);

    // 提交无返回值任务。
    bool Post(const std::function<void()>& fnTask);

    // 停止并等待任务完成。
    void Stop();

    // 是否正在运行。
    bool IsRunning() const;

private:
    std::unique_ptr<common::CThreadPool> m_pPool;
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
        [pExecutor, pNextState, f](const CTaskResult<T>& upResult)
        {
            // ① 上游失败：错误传播给下游
            if (upResult.Failed())
            {
                pNextState->Complete(CTaskResult<R>::Failure(
                    upResult.Error().nCode, upResult.Error().strMessage));
                return;
            }
            // ② 拷贝值，供异步续接安全使用
            T valueCopied = upResult.Value();
            std::function<void()> fnRun = [pNextState, f, valueCopied]()
            {
                try
                {
                    detail::CompleteSuccess(pNextState,
                        [valueCopied, f]() { return f(valueCopied); });
                }
                catch (const std::exception& e)
                {
                    pNextState->Complete(CTaskResult<R>::Failure(kTaskFailed, e.what()));
                }
                catch (...)
                {
                    pNextState->Complete(CTaskResult<R>::Failure(kTaskFailed, "unknown error"));
                }
            };
            // ③ 在执行器上执行；无执行器时内联执行
            if (pExecutor != nullptr)
            {
                if (!pExecutor->Post(fnRun))
                {
                    pNextState->Complete(CTaskResult<R>::Failure(
                        kExecutorStopped, "CAsyncExecutor 已停止"));
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
        [fnCallback](const CTaskResult<T>& result)
        {
            if (result.Ok() && fnCallback)
            {
                fnCallback(result.Value());
            }
        });
}

template <typename T>
void CTask<T>::OnFailure(const std::function<void(const CError&)>& fnCallback)
{
    m_pState->AddContinuation(
        [fnCallback](const CTaskResult<T>& result)
        {
            if (result.Failed() && fnCallback)
            {
                fnCallback(result.Error());
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
            detail::CompleteSuccess(pState, f);
        }
        catch (const std::exception& e)
        {
            pState->Complete(CTaskResult<R>::Failure(kTaskFailed, e.what()));
        }
        catch (...)
        {
            pState->Complete(CTaskResult<R>::Failure(kTaskFailed, "unknown error"));
        }
    };
    if (!m_pPool || !m_pPool->Submit(fnRun))
    {
        pState->Complete(CTaskResult<R>::Failure(
            kExecutorNotStarted, "CAsyncExecutor 未启动"));
    }
    return task;
}

} // namespace nothrow
} // namespace common
