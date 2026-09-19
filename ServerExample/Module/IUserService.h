#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
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


/// @brief 用户业务的失败（业务自定义的异常类型）。
///
/// 拒绝统一用标准异常表达（框架只搬运、不解释）；业务要按种类分流时，
/// 就定义自己的异常类型并在里面带上**业务自己的**种类 —— 框架侧没有任何错误码。
class CUserError : public std::runtime_error
{
public:
    /// @brief 失败种类（业务自己的概念）。
    enum EKind
    {
        kInvalidParam,    ///< 入参非法（id / 用户名）。
        kNotFound,        ///< 用户不存在。
        kDuplicate,       ///< 用户已存在（重复注册）。
        kDbUnavailable,   ///< 数据访问失败 / 不可用。
        kVersionConflict  ///< 乐观锁重试次数用尽。
    };

    /// @brief 构造。
    ///
    /// @param eKind 失败种类。
    /// @param strWhat 异常描述（日志 / 审计用）。
    CUserError(EKind eKind, const std::string& strWhat) : std::runtime_error(strWhat), m_eKind(eKind)
    {}

    /// @brief 失败种类。
    ///
    /// @return 种类。
    EKind Kind() const
    {
        return m_eKind;
    }

private:
    EKind m_eKind;  ///< 失败种类。
};

/// @brief 提取拒绝原因里的用户业务失败（**不是** `CUserError` → 返回 false）。
///
/// @param result 待看的结果。
/// @param eKindOut 输出：失败种类（返回 true 时有效）。
/// @param strWhatOut 输出：异常描述。
///
/// @return true 是用户业务失败。
inline bool TryGetUserError(const common::async::CPromiseResult& result, CUserError::EKind& eKindOut, std::string& strWhatOut)
{
    if (result.IsFulfilled())
    {
        return false;
    }
    try
    {
        std::rethrow_exception(result.Exception());
    }
    catch (const CUserError& e)
    {
        eKindOut = e.Kind();
        strWhatOut = e.what();
        return true;
    }
    catch (...)
    {
        return false;  // 框架侧失败或其他异常：交给调用方透传
    }
    return false;
}

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
/// 拒绝：见 `CUserError`（业务异常）；跨模块的数据访问失败在这里被翻译成业务异常，
/// 框架侧失败（执行器已停 / 处理器异常）原样透传。
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
    virtual common::async::CPromise<CUserOpContext> RenameUserAsync(std::uint64_t nUserId, const std::string& strNewName) = 0;

    // 异步删除用户（删）。
    virtual common::async::CPromise<CUserOpContext> RemoveUserAsync(std::uint64_t nUserId) = 0;
};

}  // namespace serverexample
