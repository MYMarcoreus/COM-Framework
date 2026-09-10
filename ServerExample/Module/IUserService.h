#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "Module/IUnknown.h"
#include "Module/IUserTable.h"
#include "Module/InterfaceId.h"

namespace serverexample {

/// @brief 用户业务操作上下文（一次业务调用共用一个实例）。
///
/// 纯数据载体：不持有模块与接口（依赖由业务模块持有并按值传给流程），
/// 便于观察与排障（strTrace 记录业务流程走向）。跨模块调用的数据放在 spDbOp 里。
struct CUserOpContext
{
    std::uint64_t nUserId;                 ///< 目标用户 id（注册成功后为分配到的 id）。
    CUserRecord recRequest;                ///< 业务入参（新增 / 改名）。
    CUserRecord recResult;                 ///< 业务结果（查询命中 / 写入后回读）。
    std::shared_ptr<CUserTableOp> spDbOp;  ///< 本次跨模块调用（数据访问模块）的上下文。
    bool bExists;                          ///< 数据访问层是否命中记录（业务判断用）。
    int nAttempt;                          ///< 乐观锁尝试次数（1 表示一次成功）。
    std::string strAction;                 ///< 操作名（审计 / 日志用）。
    std::string strTrace;                  ///< 业务层执行轨迹。
    std::string strError;                  ///< 失败描述（与拒绝码对应，便于排障）。

    CUserOpContext() : nUserId(0), spDbOp(new CUserTableOp()), bExists(false), nAttempt(0)
    {}
};

/// @brief 用户业务错误码（业务错误码从 kBusinessBase 起取）。
enum UserServiceCode
{
    kUserInvalidParam = common::async::kBusinessBase + 11,    ///< 入参非法（id / 用户名）。
    kUserNotFound = common::async::kBusinessBase + 12,        ///< 用户不存在。
    kUserDuplicate = common::async::kBusinessBase + 13,       ///< 用户已存在（重复注册）。
    kUserDbUnavailable = common::async::kBusinessBase + 14,   ///< 数据访问失败 / 不可用。
    kUserVersionConflict = common::async::kBusinessBase + 15  ///< 乐观锁重试次数用尽。
};

/// @brief 用户业务接口标识。
inline const sc::InterfaceId& IID_IUserService()
{
    static const sc::InterfaceId iid("serverexample::IUserService", "dabc594c-509a-4c62-9d1a-723e06b6ead3");
    return iid;
}

/// @brief 用户业务服务（模拟真实业务：模块向外部提供多个异步函数）。
///
/// 每个方法都是**异步函数**：立即返回 promise 句柄（首层已投递到业务模块执行器），
/// 调用方（其他模块 / 应用层）用 `Then` / `Catch` / `Finally` / `OnSettled` 接管后续 ——
/// **全程不需要阻塞等待**；操作数据从 `promise.GetContext()` 取。
///
/// 拒绝码：见 UserServiceCode；框架码（kStopped / kException）与跨模块码在这里被
/// 映射为业务码后透传。
class IUserService : public virtual sc::IUnknown
{
   public:
    virtual ~IUserService()
    {}

    // 异步查询用户信息（读）。
    virtual common::async::CPromise<CUserOpContext> QueryUserAsync(std::uint64_t nUserId) = 0;

    // 异步注册用户（写；recRequest.nUserId 为 0 时由数据访问层自增分配）。
    virtual common::async::CPromise<CUserOpContext> RegisterUserAsync(const CUserRecord& recRequest) = 0;

    // 异步修改用户名（改；乐观锁冲突在回调里自动重试）。
    virtual common::async::CPromise<CUserOpContext> RenameUserAsync(std::uint64_t nUserId,
                                                                    const std::string& strNewName) = 0;

    // 异步删除用户（删）。
    virtual common::async::CPromise<CUserOpContext> RemoveUserAsync(std::uint64_t nUserId) = 0;
};

}  // namespace serverexample
