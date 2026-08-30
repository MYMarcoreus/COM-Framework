#pragma once

#include <functional>
#include <mutex>
#include <vector>

namespace sc {

/// @brief 业务流程回调栈。
///
/// 贯穿一次业务处理：处理过程中可在线程池线程上压栈回调（线程安全），
/// 业务处理结束时逐个出栈（LIFO）触发。
/// 用途：资源释放、事务收尾、审计日志、结果汇总、跨模块后处理等。
///
/// 线程安全：Push / Size / Empty / RunAll / Clear 可跨线程调用。
class CCallbackStack
{
public:
    /// @brief 压栈回调（越晚压入越先触发）。
    ///
    /// @param fnCallback 回调（可为空，空则忽略）。
    void Push(const std::function<void()>& fnCallback);

    /// @brief 压栈回调（移动版，避免 std::function 拷贝）。
    void Push(std::function<void()>&& fnCallback);

    /// @brief 当前栈深。
    size_t Size() const;

    /// @brief 是否为空。
    bool Empty() const;

    /// @brief 出栈触发全部（LIFO），触发后清空。
    ///
    /// 单个回调异常被捕获并记录日志，不影响其余回调；
    /// 回调在锁外执行，可安全压入新回调（不会在本轮触发）。
    void RunAll();

    /// @brief 清空且不触发（异常回滚 / 流程丢弃时使用）。
    void Clear();

private:
    std::vector<std::function<void()>> m_vecCallbacks; // 回调栈（尾为栈顶）。
    mutable std::mutex m_mutex;                        // 保护回调栈。
};

} // namespace sc
