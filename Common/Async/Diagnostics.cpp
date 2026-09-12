#include "Async/Diagnostics.h"

#include <cstdio>
#include <mutex>

#include "Assert.h"

// ====================================================================
// 诊断钩子实现（进程级单例；报告路径必须自身安全）
//
// 报告可能发生在任意线程（工作线程 / 结算线程 / 调用线程），所以槽位加锁读写；
// 真正调用处理器时**不持锁**（处理器可能接日志，日志可能阻塞）。
// ====================================================================

namespace common {
namespace async {

namespace {

/// @brief 诊断处理器槽（进程级；进程内共享一个，故加锁保护）。
std::mutex& DiagnosticMutex()
{
    static std::mutex s_mutex;
    return s_mutex;
}

/// @brief 当前诊断处理器（空 = 未设置，走默认策略）。
DiagnosticHandler& DiagnosticSlot()
{
    static DiagnosticHandler s_fnHandler;
    return s_fnHandler;
}

}  // namespace

/// @brief 设置诊断处理器（线程安全）。
///
/// @param fnHandler 处理器；传 nullptr 恢复默认（debug 构建打印到 stderr，发布构建忽略）。
void SetDiagnosticHandler(const DiagnosticHandler& fnHandler)
{
    std::lock_guard<std::mutex> lock(DiagnosticMutex());
    DiagnosticSlot() = fnHandler;
}

/// @brief 报告一次诊断（框架内部用；没设处理器时按默认策略处理，自身不抛异常）。
///
/// @param strWhat 问题描述。
void ReportDiagnostic(const char* strWhat)
{
    DiagnosticHandler fnHandler;
    {
        std::lock_guard<std::mutex> lock(DiagnosticMutex());
        fnHandler = DiagnosticSlot();
    }

    if (fnHandler)
    {
        try
        {
            fnHandler(strWhat != nullptr ? strWhat : "(null)");
        }
        catch (...)
        {
            // 诊断处理器自己抛异常：忽略（报告问题的手段不能反过来弄坏框架）。
        }
        return;
    }

#if FRAMEWORK_DEBUG
    // 默认策略：调试构建打印（让开发期一眼看到误用），发布构建安静。
    // 调试判定只有一处：Common/Assert.h 的 FRAMEWORK_DEBUG（与 ASYNC_LOC、ASSERT 一致）。
    std::fprintf(stderr, "[async] %s\n", strWhat != nullptr ? strWhat : "(null)");
#endif
}

}  // namespace async
}  // namespace common
