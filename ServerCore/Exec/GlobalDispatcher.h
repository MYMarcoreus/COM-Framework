#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Exec/BusinessFlow.h"
#include "Exec/ModuleScheduler.h"
#include "Thread/ThreadPool.h"

namespace sc {

/// @brief 全局业务调度器：唯一向全局线程池投递业务任务的入口。
///
/// 职责：
///  - 持有全局线程池（只执行不调度）；
///  - 注册/维护各模块的读写调度器（CModuleScheduler）；
///  - 接收业务请求，创建业务流程（含回调栈），投递流程主体到线程池；
///  - 流程主体结束并排空全部子任务后，回放流程回调栈（LIFO）。
///
/// 线程安全：RegisterScheduler / UnregisterScheduler / FindScheduler /
/// Dispatch 可跨线程调用。
class CGlobalDispatcher
{
public:
    /// @brief 构造。
    ///
    /// @param pPool 全局线程池（仅执行不调度；生命周期由调用方管理）。
    explicit CGlobalDispatcher(common::thread::CThreadPool* pPool);

    virtual ~CGlobalDispatcher();

    /// @brief 注册模块调度器（模块 Start 时调用）。
    ///
    /// @param strModule 模块名（查找键）。
    /// @param pScheduler 模块调度器（生命周期由模块持有，注册期不得析构）。
    /// @return true 注册成功；false 调度器为空。
    bool RegisterScheduler(const std::string& strModule, CModuleScheduler* pScheduler);

    /// @brief 注销模块调度器（模块 Stop 时调用）。
    void UnregisterScheduler(const std::string& strModule);

    /// @brief 查找模块调度器（线程安全；找不到返回 nullptr）。
    CModuleScheduler* FindScheduler(const std::string& strModule) const;

    /// @brief 投递一个完整业务流程（线程安全）。
    ///
    /// 流程以 shared_ptr 传给主体与子任务，保证流程跨线程存活；
    /// 主体结束后若全部子任务排空则回放回调栈。
    ///
    /// @param fnBody 流程主体（在线程池线程执行；接收流程自引用）。
    /// @return true 已投递；false 线程池不可用或主体为空。
    bool Dispatch(const std::function<void(const std::shared_ptr<CBusinessFlow>&)>& fnBody);

    /// @brief 等待所有已注册模块调度器排空（Shutdown 时由编排线程调用）。
    void DrainAll();

    /// @brief 全局线程池（供 CModuleScheduler 重投递使用）。
    common::thread::CThreadPool* Pool() const { return m_pPool; }

private:
    common::thread::CThreadPool* m_pPool; // 全局线程池（生命周期由调用方管理）。
    mutable std::mutex m_mutex;           // 保护模块调度器注册表。
    std::unordered_map<std::string, CModuleScheduler*> m_mapSchedulers;
};

} // namespace sc
