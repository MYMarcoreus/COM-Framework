#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "Module/Module.h"
#include "Thread/ThreadPool.h"

namespace sc {

/// @brief 获取 IThreadPool 接口标识。
inline const InterfaceId& IID_IThreadPool()
{
    static const InterfaceId iid("sc::IThreadPool", "8f578de1-43a1-46fd-a371-2766edbb7f32");
    return iid;
}

/// @brief 线程池接口（模块化适配 common::CThreadPool）。
///
/// 使模块通过模块管理器按接口使用线程池执行任务。
class IThreadPool : public virtual IUnknown
{
public:
    virtual ~IThreadPool() {}

    // 启动工作线程。
    virtual bool Start() = 0;

    // 提交任务（无返回值）。
    virtual bool Submit(const std::function<void()>& task) = 0;

    // 停止并等待任务完成。
    virtual void Stop() = 0;
};

/// @brief 线程池模块。
///
/// 内部持有 common::CThreadPool 实例。
class CThreadPoolModule : public CModule, public IThreadPool
{
public:
    explicit CThreadPoolModule(size_t nThreadCount = 1);

    virtual ~CThreadPoolModule();

    bool Start() override;
    bool Submit(const std::function<void()>& task) override;
    void Stop() override;

protected:
    // 接口查询实现。
    bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv) override;

private:
    std::unique_ptr<common::CThreadPool> m_pPool;
    size_t m_nThreadCount;
};

} // namespace sc
