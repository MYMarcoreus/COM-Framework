#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "Async/AsyncExecutor.h"
#include "Infra/IAsyncExecutor.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"

namespace sc {

/// @brief 异步执行器模块。
///
/// 内部持有 common::CAsyncExecutor 实例。
class CAsyncExecutorModule : public CModule, public IAsyncExecutor
{
public:
    explicit CAsyncExecutorModule(size_t nThreadCount = 1);

    virtual ~CAsyncExecutorModule();

    bool Initialize(const CResolveContext& ctx) override;
    bool Start() override;
    bool Post(const std::function<void()>& task) override;
    void Stop() override;
    void Shutdown() override;

    SC_DECLARE_INTERFACE_MAP();

private:
    std::unique_ptr<common::CAsyncExecutor> m_pExecutor;
    size_t m_nThreadCount;
};

} // namespace sc
