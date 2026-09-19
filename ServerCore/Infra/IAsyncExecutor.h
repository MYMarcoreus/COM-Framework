#pragma once

#include <cstddef>
#include <functional>

#include "Async/ReadWriteGate.h"  // TaskKind（读可并发 / 写独占）
#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"

namespace sc {

/// @brief 异步执行器接口（模块化适配 common::async::CAsyncExecutor）。
///
/// 暴露无返回值任务的提交（Post）；需要 promise 链（`CPromise<TContext>`）或
/// 协程（`CCoroutine<TContext>`）时直接使用 common::async::CAsyncExecutor
/// （模板接口无法进虚函数表）。
SC_INTERFACE(IAsyncExecutor, "sc::IAsyncExecutor", "c71a0b68-66ef-47b2-8a52-64404059daf0")
{
public:
    virtual ~IAsyncExecutor()
    {}

    // 启动工作线程。
    virtual bool Start() = 0;

    // 提交无返回值任务。
    //
    // 类别必填：`kWrite` = 与模块内其它任务互斥，`kRead` = 可与其它读任务并发进入模块
    // （读任务不得修改模块状态）。
    virtual bool Post(common::async::TaskKind eKind, const std::function<void()>& task) = 0;

    // 停止并等待任务完成。
    virtual void Stop() = 0;
};

}  // namespace sc
