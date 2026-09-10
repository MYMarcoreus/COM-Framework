#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "Async/AsyncExecutor.h"
#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "Module/IUserTable.h"
#include "Module/InterfaceMap.h"
#include "Module/Module.h"

namespace serverexample {

/// @brief 模拟数据库模块（用户信息表的数据访问）。
///
/// 模拟真实数据访问层的三个要点：
///  - 表数据放内存 map + 互斥锁（模拟数据库行 / 连接资源竞争）；
///  - 层内 sleep 模拟磁盘 / 网络 IO 延迟（由 `db.latency_ms` 配置）；
///  - **本模块内的异步函数互相复用**：读表由内部异步函数 LoadRowAsync 提供，
///    插入 / 更新 / 删除都在它的 promise 上追加 handler（Then / Catch / Finally），
///    全程非阻塞；
///  - 层内异常（模拟驱动故障）由框架捕获转为 kException，不向调用方抛出。
///
/// 模块名 "example-db"，实现接口 IUserTable。
/// 自建独立执行器：其他模块调用本模块的异步函数时，本模块的层跑在自己的线程池上，
/// 调用方不需要等待（只登记回调），因此双方互相不占线程。
class CExampleDbModule : public sc::CModule, public IUserTable
{
   public:
    explicit CExampleDbModule(int nLatencyMs);

    virtual ~CExampleDbModule();

    // 生命周期：无外部依赖，Initialize 仅校验入参。
    bool Initialize(const sc::CResolveContext& ctx) override;

    // 装载种子数据并启动自建执行器。
    bool Start() override;

    // 停止执行器（等待已投递任务完成，保证层不再访问本模块）。
    void Stop() override;

    // 停止并清空表数据。
    void Shutdown() override;

    // 状态报告（当前表行数）。
    std::string GetStatus() const override;

   protected:
    // 接口查询实现（暴露自定义接口 IUserTable）。
    SC_DECLARE_INTERFACE_MAP();

   private:
    // 处理器类型（then / catch / finally 的回调，固定签名）。
    using Handler = common::async::CPromise<CUserTableOp>::ThenHandler;
    // 成员函数形式的处理器（可访问表数据与互斥锁）。
    using HandlerMemberFn = common::async::CPromiseResult (CExampleDbModule::*)(
        common::async::CPromiseResult upResult, const std::shared_ptr<CUserTableOp>& spOp);

    // ---------------- 对外异步函数（IUserTable 实现） ----------------
    common::async::CPromise<CUserTableOp> QueryUserAsync(const std::shared_ptr<CUserTableOp>& spOp) override;
    common::async::CPromise<CUserTableOp> InsertUserAsync(const std::shared_ptr<CUserTableOp>& spOp) override;
    common::async::CPromise<CUserTableOp> UpdateUserAsync(const std::shared_ptr<CUserTableOp>& spOp) override;
    common::async::CPromise<CUserTableOp> DeleteUserAsync(const std::shared_ptr<CUserTableOp>& spOp) override;

    // ---------------- 本模块内的异步函数 ----------------
    // 读表（取连接 + 读行）；被查询 / 插入 / 更新 / 删除复用（同上下文类型，直接追加 handler）。
    common::async::CPromise<CUserTableOp> LoadRowAsync(const std::shared_ptr<CUserTableOp>& spOp);

    // ---------------- 处理器（本模块内的步骤） ----------------
    // 取连接 + 模拟 IO 延迟。
    common::async::CPromiseResult StepAcquireConn(common::async::CPromiseResult upResult,
                                                  const std::shared_ptr<CUserTableOp>& spOp);
    // 读表（未命中 → kDbRowNotFound；演示开关打开时抛异常模拟驱动故障）。
    common::async::CPromiseResult StepLoadRow(common::async::CPromiseResult upResult,
                                              const std::shared_ptr<CUserTableOp>& spOp);
    // catch：把「记录不存在」归一化为兑现，供写入流程继续。
    common::async::CPromiseResult StepAcceptNotFound(common::async::CPromiseResult upResult,
                                                     const std::shared_ptr<CUserTableOp>& spOp);
    // 查重（已存在 → kDbDuplicateKey）。
    common::async::CPromiseResult StepRejectIfExists(common::async::CPromiseResult upResult,
                                                     const std::shared_ptr<CUserTableOp>& spOp);
    // 写表：插入行（必要时分配自增 id）。
    common::async::CPromiseResult StepInsertRow(common::async::CPromiseResult upResult,
                                                const std::shared_ptr<CUserTableOp>& spOp);
    // 写表：乐观锁更新（版本不匹配 → kDbVersionConflict，并把库中最新行写入 recResult）。
    common::async::CPromiseResult StepApplyUpdate(common::async::CPromiseResult upResult,
                                                  const std::shared_ptr<CUserTableOp>& spOp);
    // 写表：删除行。
    common::async::CPromiseResult StepEraseRow(common::async::CPromiseResult upResult,
                                               const std::shared_ptr<CUserTableOp>& spOp);
    // finally：模拟释放连接（无论成败都执行；返回值被 finally 忽略，结果原样透传）。
    common::async::CPromiseResult StepReleaseConn(common::async::CPromiseResult upResult,
                                                  const std::shared_ptr<CUserTableOp>& spOp);

    // 绑定成员函数为处理器（处理器内可直接访问表数据）。
    Handler BindHandler(HandlerMemberFn pfnHandler);

    // 创建无效 promise（模块未启动 / 入参为空时返回，调用方 Await() 得 kStopped）。
    common::async::CPromise<CUserTableOp> MakeInvalidPromise() const;

    std::unique_ptr<common::async::CAsyncExecutor> m_pExecutor;  ///< 自建执行器（promise 调度）。
    std::map<std::uint64_t, CUserRecord> m_mapRows;              ///< 表数据（m_mutex 保护）。
    mutable std::mutex m_mutex;                                  ///< 保护表数据。
    std::uint64_t m_nNextId;                                     ///< 自增主键游标。
    int m_nLatencyMs;                                            ///< 模拟 IO 延迟（毫秒）。
};

}  // namespace serverexample
