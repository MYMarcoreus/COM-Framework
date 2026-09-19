#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "Module/IUnknown.h"
#include "Module/InterfaceId.h"

namespace serverexample {

/// @brief 用户信息表记录（模拟数据库中的一行）。
struct CUserRecord
{
    std::uint64_t nUserId;  ///< 主键（用户 id）。
    std::string strName;    ///< 用户名。
    std::string strMail;    ///< 邮箱。
    int nLevel;             ///< 用户等级。
    int nVersion;           ///< 乐观锁版本号（每次更新 +1）。

    CUserRecord() : nUserId(0), nLevel(1), nVersion(1)
    {}
};

/// @brief 数据访问操作上下文（查询 / 插入 / 更新 / 删除共用一个实例）。
///
/// 与 promise 模型一致：处理器之间只传「兑现 / 拒绝」（CPromiseResult），
/// 数据全部写在这里；同一实例贯穿本次操作（由调用方创建并持有），
/// 操作结束后调用方从本对象取结果。一次操作只被一条 promise 链顺序访问，
/// 因此本结构内部无需再加锁。
struct CUserTableOp
{
    std::uint64_t nUserId;   ///< 目标用户 id（查询 / 删除用；插入时为 0 表示自增分配）。
    CUserRecord recRequest;  ///< 入参记录（插入 / 更新用；更新的 nVersion 为期望版本）。
    CUserRecord recResult;   ///< 结果记录（查询命中 / 写入后回读；版本冲突时为库中最新行）。
    bool bFound;             ///< 是否命中记录（读表层写入，供后续层判断）。
    bool bSimulateDbError;   ///< 演示开关：true 时读表层抛出异常（模拟数据库驱动故障）。
    std::string strTrace;    ///< 层执行轨迹（观察流程走向与排障用）。

    CUserTableOp() : nUserId(0), bFound(false), bSimulateDbError(false)
    {}
};


/// @brief 用户信息表接口标识。
inline const sc::InterfaceId& IID_IUserTable()
{
    static const sc::InterfaceId iid("serverexample::IUserTable", "388a310a-1cdc-4d6a-bb4e-eadec37b41ff");
    return iid;
}

/// @brief 数据访问层的业务失败（业务自定义的异常类型）。
///
/// 拒绝统一用标准异常表达（框架只搬运、不解释）；业务要按种类分流时，
/// 就定义自己的异常类型并在里面带上「业务自己的」种类 —— 框架侧没有任何错误码。
class CDbError : public std::runtime_error
{
public:
    /// @brief 失败种类（业务自己的概念）。
    enum EKind
    {
        kRowNotFound,     ///< 记录不存在（查询 / 删除未命中）。
        kDuplicateKey,    ///< 主键冲突（重复插入）。
        kVersionConflict  ///< 乐观锁版本冲突（recResult 为库中最新行，便于重试）。
    };

    /// @brief 构造。
    ///
    /// @param eKind 失败种类。
    /// @param strWhat 异常描述（日志与人读）。
    CDbError(EKind eKind, const std::string& strWhat) : std::runtime_error(strWhat), m_eKind(eKind)
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

/// @brief 提取拒绝原因里的数据访问层失败（「不是」 `CDbError` → 返回 false）。
///
/// 这是「按异常类型分流」的标准写法：对结果里的异常对象做 `dynamic_cast`（不走 `catch`、不抛不捕）；
/// 其余（兑现 / 框架侧失败 / 其他业务异常）原封不动地继续透传。
///
/// @param result 待看的结果。
/// @param eKindOut 输出：失败种类（返回 true 时有效）。
/// @param strWhatOut 输出：异常描述。
///
/// @return true 是数据访问层业务失败。
inline bool TryGetDbError(const common::async::CPromiseResult& result, CDbError::EKind& eKindOut, std::string& strWhatOut)
{
    const CDbError* pError = dynamic_cast<const CDbError*>(result.Exception().get());
    if (pError == nullptr)
    {
        return false;  // 兑现 / 框架侧失败 / 其他业务异常：交给调用方透传
    }
    eKindOut = pError->Kind();
    strWhatOut = pError->what();
    return true;
}

/// @brief 用户信息表（模拟数据库的数据访问模块接口）。
///
/// 四个方法都是「异步函数」：立即返回 promise 句柄（命名对齐 JS Promise），
/// 调用方用 `Then` / `Catch` / `Finally` / `OnSettled` 接管后续 —— 调用方
/// 「不需要阻塞等待」，本模块的层跑在自己的执行器上，操作上下文由调用方提供，
/// 结果写回同一实例。
///
/// 拒绝（沿 promise 链透传到调用方的 catch / OnSettled，全部是标准异常）：
///  - 查询 / 删除未命中 → `CDbError(kRowNotFound)`；
///  - 插入主键冲突 → `CDbError(kDuplicateKey)`；
///  - 更新版本不匹配 → `CDbError(kVersionConflict)`（recResult 为库中最新行，便于重试）；
///  - 驱动异常 → 抛出后被框架原样收口为拒绝（异常类型与 what() 都不丢）。
class IUserTable : public virtual sc::IUnknown
{
public:
    virtual ~IUserTable()
    {}

    // 异步查询用户（未命中以 CDbError(kRowNotFound) 拒绝）。
    virtual common::async::CPromise<CUserTableOp> QueryUserAsync(const std::shared_ptr<CUserTableOp>& spOp) = 0;

    // 异步插入用户（nUserId 为 0 时自增分配；主键冲突以 CDbError(kDuplicateKey) 拒绝）。
    virtual common::async::CPromise<CUserTableOp> InsertUserAsync(const std::shared_ptr<CUserTableOp>& spOp) = 0;

    // 异步更新用户（乐观锁：recRequest.nVersion 为期望版本，冲突时 recResult 为库中最新行）。
    virtual common::async::CPromise<CUserTableOp> UpdateUserAsync(const std::shared_ptr<CUserTableOp>& spOp) = 0;

    // 异步删除用户（未命中以 CDbError(kRowNotFound) 拒绝）。
    virtual common::async::CPromise<CUserTableOp> DeleteUserAsync(const std::shared_ptr<CUserTableOp>& spOp) = 0;
};

}  // namespace serverexample
