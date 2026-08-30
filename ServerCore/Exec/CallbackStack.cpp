#include "Exec/CallbackStack.h"

#include <exception>
#include <string>
#include <utility>

#include "Log/Logger.h"

namespace sc {

void CCallbackStack::Push(const std::function<void()>& fnCallback)
{
    if (!fnCallback)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vecCallbacks.push_back(fnCallback);
}

void CCallbackStack::Push(std::function<void()>&& fnCallback)
{
    if (!fnCallback)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vecCallbacks.push_back(std::move(fnCallback));
}

size_t CCallbackStack::Size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_vecCallbacks.size();
}

bool CCallbackStack::Empty() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_vecCallbacks.empty();
}

void CCallbackStack::RunAll()
{
    // 取出全部回调后锁外执行，避免回调中再次 Push 时自锁。
    std::vector<std::function<void()>> vecCallbacks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        vecCallbacks.swap(m_vecCallbacks);
    }
    // LIFO：栈顶（尾部）先执行；单个回调异常不影响其余。
    for (std::vector<std::function<void()>>::reverse_iterator it = vecCallbacks.rbegin();
         it != vecCallbacks.rend(); ++it)
    {
        if (!(*it))
        {
            continue;
        }
        try
        {
            (*it)();
        }
        catch (const std::exception& e)
        {
            common::log::CLogger::Instance().Error(
                std::string("CCallbackStack::RunAll 回调异常: ") + e.what());
        }
        catch (...)
        {
            common::log::CLogger::Instance().Error("CCallbackStack::RunAll 回调未知异常");
        }
    }
}

void CCallbackStack::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vecCallbacks.clear();
}

} // namespace sc
