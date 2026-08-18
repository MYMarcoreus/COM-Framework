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

class AsyncExecutor;

namespace detail {

/// @brief 任务共享状态：结果/异常 + 续接列表。
///
/// 续接在状态就绪时触发，成功与异常分开处理，保证链式传播。
template <typename T>
class TaskState
{
public:
    /// @brief 续接回调：value 为 nullptr 表示异常。
    using Continuation = std::function<void(const T* value, const std::exception_ptr& eptr)>;

    TaskState() : ready_(false) {}

    // 提供 future（供 Get 阻塞等待）。
    std::future<T> GetFuture() { return promise_.get_future(); }

    // 成功完成并触发续接。
    void CompleteSuccess(const T& value)
    {
        std::vector<Continuation> cbs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ready_)
            {
                return;
            }
            ready_ = true;
            value_ = std::make_shared<T>(value);
            cbs.swap(continuations_);
        }
        promise_.set_value(value);
        for (size_t i = 0; i < cbs.size(); ++i)
        {
            if (cbs[i])
            {
                cbs[i](value_.get(), nullptr);
            }
        }
    }

    // 异常完成并触发续接。
    void CompleteFailure(const std::exception_ptr& eptr)
    {
        std::vector<Continuation> cbs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ready_)
            {
                return;
            }
            ready_ = true;
            exception_ = eptr;
            cbs.swap(continuations_);
        }
        promise_.set_exception(eptr);
        for (size_t i = 0; i < cbs.size(); ++i)
        {
            if (cbs[i])
            {
                cbs[i](nullptr, eptr);
            }
        }
    }

    // 注册续接；若已就绪则立即触发。
    void AddContinuation(const Continuation& cb)
    {
        const T* value = nullptr;
        std::exception_ptr eptr;
        bool fireNow = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ready_)
            {
                fireNow = true;
                value = value_.get();
                eptr = exception_;
            }
            else
            {
                continuations_.push_back(cb);
            }
        }
        if (fireNow && cb)
        {
            cb(value, eptr);
        }
    }

private:
    std::promise<T> promise_;
    mutable std::mutex mutex_;
    std::vector<Continuation> continuations_;
    bool ready_;
    std::shared_ptr<T> value_;
    std::exception_ptr exception_;
};

} // namespace detail

/// @brief 异步任务，支持链式调用（Then）。
///
/// 由 AsyncExecutor::Submit / Task::FromResult 创建，
/// 通过 Then 串联后续步骤，Get 阻塞获取最终结果。
template <typename T>
class Task
{
public:
    // 创建空任务。
    Task() : executor_(nullptr), state_(std::make_shared<detail::TaskState<T> >()) {}

    // 从已就绪的值创建任务（可启动链式调用）。
    static Task<T> FromResult(const T& value)
    {
        Task<T> task;
        task.state_->CompleteSuccess(value);
        return task;
    }

    // 链式续接：本任务成功后，在执行器上运行 f(value)。
    template <typename F>
    Task<typename std::result_of<F(T)>::type> Then(F f);

    // 阻塞获取结果；失败时抛出异常。
    T Get() const { return state_->GetFuture().get(); }

    // 注册成功回调（fire-and-forget）。
    void OnSuccess(const std::function<void(const T&)>& cb);

    // 注册失败回调（fire-and-forget）。
    void OnFailure(const std::function<void(const std::exception_ptr&)>& cb);

private:
    friend class AsyncExecutor;
    template <typename U> friend class Task;

    AsyncExecutor* executor_;
    std::shared_ptr<detail::TaskState<T> > state_;
};

/// @brief 异步执行器。
///
/// 基于线程池执行任务，支持链式调用（Submit → Then → Get）。
///
/// @note executor 必须存续到所有任务完成后再销毁。
class AsyncExecutor
{
public:
    // 创建执行器（指定工作线程数）。
    explicit AsyncExecutor(size_t threadCount = 1);

    ~AsyncExecutor();

    // 启动工作线程。
    bool Start();

    // 提交任务并返回 Task。
    template <typename F>
    Task<typename std::result_of<F()>::type> Submit(F f);

    // 提交无返回值任务。
    bool Post(const std::function<void()>& task);

    // 停止并等待任务完成。
    void Stop();

    // 是否正在运行。
    bool IsRunning() const;

private:
    std::unique_ptr<ThreadPool> pool_;
    size_t threadCount_;
};

// ================= 模板实现 ================

template <typename T>
template <typename F>
Task<typename std::result_of<F(T)>::type> Task<T>::Then(F f)
{
    using R = typename std::result_of<F(T)>::type;
    Task<R> next;
    next.executor_ = executor_;
    std::shared_ptr<detail::TaskState<R> > nextState = next.state_;
    AsyncExecutor* executor = executor_;

    state_->AddContinuation(
        [executor, nextState, f](const T* value, const std::exception_ptr& eptr)
        {
            // ① 上游失败：异常传播给下游
            if (eptr)
            {
                nextState->CompleteFailure(eptr);
                return;
            }
            // ② 拷贝值，供异步续接安全使用
            T copied = *value;
            std::function<void()> run = [nextState, f, copied]()
            {
                try
                {
                    nextState->CompleteSuccess(f(copied));
                }
                catch (...)
                {
                    nextState->CompleteFailure(std::current_exception());
                }
            };
            // ③ 在 executor 上执行；无 executor 时内联执行
            if (executor != nullptr)
            {
                if (!executor->Post(run))
                {
                    nextState->CompleteFailure(std::make_exception_ptr(
                        std::runtime_error("AsyncExecutor 已停止")));
                }
            }
            else
            {
                run();
            }
        });
    return next;
}

template <typename T>
void Task<T>::OnSuccess(const std::function<void(const T&)>& cb)
{
    state_->AddContinuation(
        [cb](const T* value, const std::exception_ptr& eptr)
        {
            if (!eptr && cb)
            {
                cb(*value);
            }
        });
}

template <typename T>
void Task<T>::OnFailure(const std::function<void(const std::exception_ptr&)>& cb)
{
    state_->AddContinuation(
        [cb](const T*, const std::exception_ptr& eptr)
        {
            if (eptr && cb)
            {
                cb(eptr);
            }
        });
}

template <typename F>
Task<typename std::result_of<F()>::type> AsyncExecutor::Submit(F f)
{
    using R = typename std::result_of<F()>::type;
    Task<R> task;
    task.executor_ = this;
    std::shared_ptr<detail::TaskState<R> > state = task.state_;
    std::function<void()> run = [state, f]()
    {
        try
        {
            state->CompleteSuccess(f());
        }
        catch (...)
        {
            state->CompleteFailure(std::current_exception());
        }
    };
    if (!pool_ || !pool_->Submit(run))
    {
        state->CompleteFailure(
            std::make_exception_ptr(std::runtime_error("AsyncExecutor 未启动")));
    }
    return task;
}

} // namespace common
