#include "Exec/GlobalDispatcher.h"

#include <exception>
#include <string>

#include "Log/Logger.h"

namespace sc {

/// @brief 创建全局业务调度器。
///
/// @param pPool 全局线程池（仅执行不调度；生命周期由调用方管理）。
CGlobalDispatcher::CGlobalDispatcher(common::thread::CThreadPool* pPool)
    : m_pPool(pPool)
{
}

CGlobalDispatcher::~CGlobalDispatcher()
{
}

/// @brief 注册模块调度器（模块 Start 时调用）。
bool CGlobalDispatcher::RegisterScheduler(const std::string& strModule,
                                          CModuleScheduler* pScheduler)
{
    if (pScheduler == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mapSchedulers[strModule] = pScheduler;
    return true;
}

/// @brief 注销模块调度器（模块 Stop 时调用）。
void CGlobalDispatcher::UnregisterScheduler(const std::string& strModule)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mapSchedulers.erase(strModule);
}

/// @brief 查找模块调度器（线程安全）。
CModuleScheduler* CGlobalDispatcher::FindScheduler(const std::string& strModule) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::unordered_map<std::string, CModuleScheduler*>::const_iterator it =
        m_mapSchedulers.find(strModule);
    return it == m_mapSchedulers.end() ? nullptr : it->second;
}

/// @brief 投递一个完整业务流程（线程安全）。
bool CGlobalDispatcher::Dispatch(
    const std::function<void(const std::shared_ptr<CBusinessFlow>&)>& fnBody)
{
    if (m_pPool == nullptr || !m_pPool->IsRunning() || !fnBody)
    {
        return false;
    }
    std::shared_ptr<CBusinessFlow> spFlow(new CBusinessFlow());
    return m_pPool->Submit([spFlow, fnBody]()
    {
        try
        {
            fnBody(spFlow); // 主体在线程池线程执行，投递子任务
        }
        catch (const std::exception& e)
        {
            common::log::CLogger::Instance().Error(
                std::string("CGlobalDispatcher::Dispatch 主体异常: ") + e.what());
        }
        catch (...)
        {
            common::log::CLogger::Instance().Error(
                "CGlobalDispatcher::Dispatch 主体未知异常");
        }
        spFlow->Complete(); // 主体结束；全部子任务排空后回放回调栈
    });
}

/// @brief 等待所有已注册模块调度器排空（Shutdown 时由编排线程调用）。
///
/// 先快照调度器列表、再在锁外逐个 Drain，避免持注册表锁等待
/// 而工作线程恰好需要 FindScheduler 造成死锁。
void CGlobalDispatcher::DrainAll()
{
    std::vector<CModuleScheduler*> vecSchedulers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::unordered_map<std::string, CModuleScheduler*>::iterator it =
                 m_mapSchedulers.begin(); it != m_mapSchedulers.end(); ++it)
        {
            if (it->second != nullptr)
            {
                vecSchedulers.push_back(it->second);
            }
        }
    }
    for (std::vector<CModuleScheduler*>::iterator it = vecSchedulers.begin();
         it != vecSchedulers.end(); ++it)
    {
        (*it)->Drain();
    }
}

} // namespace sc
