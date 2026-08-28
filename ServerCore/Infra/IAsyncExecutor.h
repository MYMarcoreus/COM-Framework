#pragma once

#include <cstddef>
#include <functional>

#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"

namespace sc {

/// @brief 异步执行器接口（模块化适配 common::async::CAsyncExecutor）。
///
/// 暴露无返回值任务的提交（Post）；需要 CTask<T> 链式调用时直接使用
/// common::async::CAsyncExecutor（模板接口无法进虚函数表）。
SC_INTERFACE(IAsyncExecutor, "sc::IAsyncExecutor", "c71a0b68-66ef-47b2-8a52-64404059daf0")
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

} // namespace sc
