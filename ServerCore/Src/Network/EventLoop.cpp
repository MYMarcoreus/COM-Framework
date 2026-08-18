#include "Network/EventLoop.h"

#include <cerrno>
#include <cstdint>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace sc {

/// @brief 创建事件循环。
///
/// 创建 eventfd 用于跨线程唤醒 poll。
EventLoop::EventLoop()
    : running_(false), wakeupFd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
{
    struct pollfd wakeup;
    wakeup.fd = wakeupFd_;
    wakeup.events = POLLIN;
    wakeup.revents = 0;
    // 唤醒描述符固定在索引 0
    pollfds_.push_back(wakeup);
    fdIndex_[wakeupFd_] = 0;
}

/// @brief 销毁事件循环。
EventLoop::~EventLoop()
{
    if (wakeupFd_ >= 0)
    {
        ::close(wakeupFd_);
    }
}

/// @brief 注册文件描述符。
///
/// @param fd 文件描述符。
/// @param events 关注的事件（POLLIN / POLLOUT 等）。
void EventLoop::AddFd(int fd, short events)
{
    if (fd < 0 || fdIndex_.find(fd) != fdIndex_.end())
    {
        return;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    fdIndex_[fd] = pollfds_.size();
    pollfds_.push_back(pfd);
}

/// @brief 移除文件描述符。
///
/// 使用交换删除保持数组紧凑。
///
/// @param fd 文件描述符。
void EventLoop::RemoveFd(int fd)
{
    std::map<int, size_t>::iterator it = fdIndex_.find(fd);
    if (it == fdIndex_.end())
    {
        return;
    }
    size_t index = it->second;
    size_t last = pollfds_.size() - 1;
    if (index != last)
    {
        pollfds_[index] = pollfds_[last];
        fdIndex_[pollfds_[index].fd] = index;
    }
    pollfds_.pop_back();
    fdIndex_.erase(it);
}

/// @brief 更新文件描述符关注的事件。
///
/// @param fd 文件描述符。
/// @param events 新的事件集合。
void EventLoop::UpdateEvents(int fd, short events)
{
    std::map<int, size_t>::iterator it = fdIndex_.find(fd);
    if (it == fdIndex_.end())
    {
        return;
    }
    pollfds_[it->second].events = events;
}

/// @brief 设置事件回调。
void EventLoop::SetEventCallback(const EventCallback& callback)
{
    callback_ = callback;
}

/// @brief 阻塞运行事件循环。
void EventLoop::Run()
{
    running_.store(true);
    while (running_.load())
    {
        int n = ::poll(pollfds_.data(), static_cast<nfds_t>(pollfds_.size()), -1);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        // 先收集就绪事件，再分发，避免回调中修改 pollfds_ 影响遍历
        std::vector<std::pair<int, short> > ready;
        for (size_t i = 0; i < pollfds_.size(); ++i)
        {
            short revents = pollfds_[i].revents;
            if (revents != 0)
            {
                ready.push_back(std::make_pair(pollfds_[i].fd, revents));
            }
        }

        for (size_t i = 0; i < ready.size(); ++i)
        {
            int fd = ready[i].first;
            short revents = ready[i].second;
            if (fd == wakeupFd_)
            {
                HandleWakeup();
                continue;
            }
            if (callback_)
            {
                callback_(fd, revents);
            }
        }
    }
    pollfds_.clear();
    fdIndex_.clear();
}

/// @brief 停止事件循环。
///
/// 写入唤醒字节使 poll 立即返回，可跨线程调用。
void EventLoop::Stop()
{
    running_.store(false);
    if (wakeupFd_ >= 0)
    {
        std::uint64_t one = 1;
        ssize_t ignored = ::write(wakeupFd_, &one, sizeof(one));
        (void)ignored;
    }
}

/// @brief 是否正在运行。
bool EventLoop::IsRunning() const
{
    return running_.load();
}

/// @brief 处理唤醒事件。
void EventLoop::HandleWakeup()
{
    std::uint64_t value;
    ssize_t ignored = ::read(wakeupFd_, &value, sizeof(value));
    (void)ignored;
}

} // namespace sc
