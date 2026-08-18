#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "Component/component.h"
#include "Thread/thread_pool.h"

namespace sc {

/// @brief 获取 IThreadPool 接口标识。
inline const InterfaceId& IID_IThreadPool()
{
    static const InterfaceId iid = "sc::IThreadPool";
    return iid;
}

/// @brief 线程池接口（组件化适配 common::CThreadPool）。
///
/// 使模块通过组件管理器按接口使用线程池执行任务。
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

/// @brief 线程池组件。
///
/// 内部持有 common::CThreadPool 实例。
class CThreadPoolComponent : public CComponent, public IThreadPool
{
public:
    explicit CThreadPoolComponent(size_t threadCount = 1);

    virtual ~CThreadPoolComponent();

    bool Start() override;
    bool Submit(const std::function<void()>& task) override;
    void Stop() override;

protected:
    // 接口查询实现。
    bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv) override;

private:
    std::unique_ptr<common::CThreadPool> pool_;
    size_t threadCount_;
};

} // namespace sc
