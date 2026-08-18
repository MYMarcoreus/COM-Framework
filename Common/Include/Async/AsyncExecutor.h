#pragma once

#include <functional>
#include <future>
#include <memory>
#include <utility>

#include "Thread/ThreadPool.h"

namespace common {

/// @brief 异步执行器。
///
/// 基于线程池执行异步任务，通过 std::future 获取任务结果。
class AsyncExecutor
{
public:
    // 创建执行器（指定工作线程数）。
    explicit AsyncExecutor(size_t threadCount = 1);

    ~AsyncExecutor();

    // 启动工作线程。
    bool Start();

    // 提交无返回值任务。
    bool Post(const std::function<void()>& task);

    // 提交任务并返回 future 获取结果。
    template <typename Function>
    std::future<typename std::result_of<Function()>::type> Submit(Function&& func);

    // 停止并等待任务完成。
    void Stop();

    // 是否正在运行。
    bool IsRunning() const;

private:
    ThreadPool pool_;
};

/// @brief 提交任务并返回 future。
///
/// @tparam Function 可调用对象类型。
/// @param func 任务函数。
///
/// @return 任务结果的 future。
template <typename Function>
std::future<typename std::result_of<Function()>::type>
AsyncExecutor::Submit(Function&& func)
{
    using ResultType = typename std::result_of<Function()>::type;
    std::shared_ptr<std::packaged_task<ResultType()> > task(
        new std::packaged_task<ResultType()>(std::forward<Function>(func)));
    std::future<ResultType> future = task->get_future();
    pool_.Submit([task]() { (*task)(); });
    return future;
}

} // namespace common
