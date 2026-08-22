#pragma once

#include <cstddef>
#include <functional>

#include "Module/IUnknown.h"
#include "Module/InterfaceDecl.h"

namespace sc {

/// @brief 线程池接口（模块化适配 common::CThreadPool）。
///
/// 使模块通过模块管理器按接口使用线程池执行任务。
SC_INTERFACE(IThreadPool, "sc::IThreadPool", "8f578de1-43a1-46fd-a371-2766edbb7f32")
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

} // namespace sc
