#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "Infra/ITimer.h"
#include "Module/IUserService.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"
#include "Module/ScopedInterfacePtr.h"

namespace serverexample {

/// @brief 用户资料业务模块（模拟真实业务：模块向外部提供多个异步函数）。
///
/// 对外（实现 IUserService，其他模块 / 应用层按接口调用）：
///  - QueryUserAsync    读用户信息；
///  - RegisterUserAsync 新增用户（校验 → 查重 → 落库）；
///  - RenameUserAsync   修改用户名（乐观锁 + 冲突自动重试）；
///  - RemoveUserAsync   删除用户。
/// 每个异步函数**立即返回 promise 句柄**（命名对齐 JS Promise）：调用方用
/// `Then` / `Catch` / `Finally` / `OnSettled` 接管后续 —— 不需要阻塞等待，
/// 本模块也不使用协程。
///
/// 内部编排（细节见 .cpp）：
///  - **本模块内的异步函数**复用：LoadUserAsync（读用户）被查询 / 改名 / 删除流程
///    直接串接（同上下文类型，追加 handler 即可，非阻塞）；
///  - **其他模块的异步函数**：数据访问模块（IUserTable）的读 / 写 / 改 / 删，用
///    `CPromise::New`（等价 JS `new Promise((resolve, reject) => ...)`）桥接成本流程的
///    promise，再用 `ThenPromise`（等价 JS 的「then 的处理器返回 promise 时等待」）
///    接入流程 —— 全程回调驱动、零阻塞，两个模块的线程池互不占用；
///  - 失败即停（then）、catch 归一化 / 恢复、finally 审计收尾、跨模块拒绝码语义转换，
///    都在流程里体现。
///
/// 模块名 "user-service"；自建执行器（promise 是模板，无法放进 IAsyncExecutor 虚接口）。
/// 周期演示也由纯回调驱动（零阻塞）：定时器只负责投递一次演示任务。
class CExampleAsyncModule : public sc::CModule, public IUserService
{
public:
    explicit CExampleAsyncModule(std::int64_t nIntervalMs);

    virtual ~CExampleAsyncModule();

    // 解析 ITimer 与 IUserTable（数据访问模块）接口。
    bool Initialize(const sc::CResolveContext& ctx) override;

    // 启动自建执行器并注册周期演示定时器。
    bool Start() override;

    // 取消定时器并停止执行器（等待已投递任务完成）。
    void Stop() override;

    // 停止并释放接口引用。
    void Shutdown() override;

    // 状态报告。
    std::string GetStatus() const override;

protected:
    // 接口查询实现（暴露自定义接口 IUserService）。
    SC_DECLARE_INTERFACE_MAP();

private:
    // ---------------- 对外异步函数（IUserService 实现） ----------------
    common::async::CPromise<CUserOpContext> QueryUserAsync(std::uint64_t nUserId) override;
    common::async::CPromise<CUserOpContext> RegisterUserAsync(const CUserRecord& recRequest) override;
    common::async::CPromise<CUserOpContext> RenameUserAsync(
        std::uint64_t nUserId, const std::string& strNewName) override;
    common::async::CPromise<CUserOpContext> RemoveUserAsync(std::uint64_t nUserId) override;

    // 创建业务操作上下文（设置操作名；数据访问操作上下文在构造中一并创建）。
    static std::shared_ptr<CUserOpContext> MakeContext(const std::string& strAction);

    // 定时器回调：投递一轮演示（不在定时器线程上跑业务代码）。
    void ScheduleExample();

    std::int64_t m_nIntervalMs;                       ///< 演示周期（毫秒）。
    sc::ScopedInterfacePtr<sc::ITimer> m_pTimer;      ///< 定时器接口。
    sc::ScopedInterfacePtr<IUserTable> m_pUserTable;  ///< 数据访问模块接口（跨模块异步调用）。
    std::shared_ptr<common::async::CAsyncExecutor> m_spExecutor;  ///< 自建执行器（按值传给流程，保生命周期）。
    common::timer::TimerId m_tTimerId;                            ///< 周期演示定时器 id。
};

}  // namespace serverexample
