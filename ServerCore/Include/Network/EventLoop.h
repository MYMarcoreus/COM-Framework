#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <poll.h>
#include <vector>

namespace sc {

/// @brief 基于 poll 的简单事件循环。
///
/// 提供文件描述符的注册、移除与事件分发，阻塞运行。
///
/// @note AddFd / RemoveFd / UpdateEvents 必须从事件循环线程调用。
class EventLoop
{
public:
    // 事件回调。
    using EventCallback = std::function<void(int fd, short revents)>;

    EventLoop();

    ~EventLoop();

    // 注册文件描述符及其关注事件。
    void AddFd(int fd, short events);

    // 移除文件描述符。
    void RemoveFd(int fd);

    // 更新文件描述符关注的事件。
    void UpdateEvents(int fd, short events);

    // 设置事件回调，必须在 Run 之前调用。
    void SetEventCallback(const EventCallback& callback);

    // 阻塞运行事件循环。
    void Run();

    // 停止事件循环，可跨线程调用。
    void Stop();

    // 是否正在运行。
    bool IsRunning() const;

private:
    // 处理唤醒事件。
    void HandleWakeup();

    std::vector<struct pollfd> pollfds_;
    std::map<int, size_t> fdIndex_;
    EventCallback callback_;
    std::atomic<bool> running_;
    const int wakeupFd_;
};

} // namespace sc
