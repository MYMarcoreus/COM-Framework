#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "Infra/IThreadPool.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "Thread/ThreadPool.h"

namespace sc {

/// @brief 线程池模块。
///
/// 内部持有 common::CThreadPool 实例。
class CThreadPoolModule : public CModule, public IThreadPool
{
public:
    explicit CThreadPoolModule(size_t nThreadCount = 1);

    virtual ~CThreadPoolModule();

    bool Initialize(const CResolveContext& ctx) override;
    bool Start() override;
    bool Submit(const std::function<void()>& task) override;
    void Stop() override;
    void Shutdown() override;

    SC_DECLARE_INTERFACE_MAP();

private:
    std::unique_ptr<common::CThreadPool> m_pPool;
    size_t m_nThreadCount;
};

} // namespace sc
