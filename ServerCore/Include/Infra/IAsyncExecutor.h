#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "Async/AsyncExecutor.h"
#include "Component/Component.h"

namespace sc {

/// @brief 获取 IAsyncExecutor 接口标识。
inline const InterfaceId& IID_IAsyncExecutor()
{
    static const InterfaceId iid = "sc::IAsyncExecutor";
    return iid;
}

/// @brief 异步执行器接口（组件化适配 common::AsyncExecutor）。
///
/// 暴露无返回值任务的提交（Post）；需要 Task<T> 链式调用时直接使用
/// common::AsyncExecutor（模板接口无法进虚函数表）。
class IAsyncExecutor : public virtual IUnknown
{
public:
    virtual ~IAsyncExecutor() {}

    // 启动工作线程。
    virtual bool Start() = 0;

    // 提交无返回值任务。
    virtual bool Post(const std::function<void()>& task) = 0;

    // 停止并等待任务完成。
    virtual void Stop() = 0;
};

/// @brief 异步执行器组件。
///
/// 内部持有 common::AsyncExecutor 实例。
class AsyncExecutorComponent : public Component, public IAsyncExecutor
{
public:
    explicit AsyncExecutorComponent(size_t threadCount = 1);

    virtual ~AsyncExecutorComponent();

    bool Start() override;
    bool Post(const std::function<void()>& task) override;
    void Stop() override;

protected:
    // 接口查询实现。
    bool QueryInterfaceImpl(const InterfaceId& iid, void** ppv) override;

private:
    std::unique_ptr<common::AsyncExecutor> executor_;
    size_t threadCount_;
};

} // namespace sc
