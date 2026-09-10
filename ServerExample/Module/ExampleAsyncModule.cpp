#include "Module/ExampleAsyncModule.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "Infra/GuardedTimer.h"
#include "Log/Logger.h"
#include "Module/ResolveContext.h"
#include "Module/ScopedInterfacePtr.h"

namespace serverexample {

namespace {

namespace no = common::async;

/// @brief 本流程的 promise 与 resolve / reject 句柄别名（减少模板噪音，便于阅读）。
using CUserPromise = no::CPromise<CUserOpContext>;
using ResolveFn = CUserPromise::ResolveFn;
using RejectFn = CUserPromise::RejectFn;

/// @brief 用户名长度上限（业务校验约束）。
const std::size_t kMaxNameLength = 32;

/// @brief 乐观锁冲突的最大尝试次数（含首次）。
const int kMaxUpdateAttempts = 3;

/// @brief 结果描述（供日志使用）。
std::string DescribeResult(const no::CPromiseResult& result)
{
    if (result.IsFulfilled())
    {
        return "兑现";
    }
    return "拒绝(码=" + std::to_string(result.Code()) + ")";
}

/// @brief 用户名合法性校验（1..kMaxNameLength 个字母 / 数字 / 下划线 / 连字符）。
///
/// @param strName 待校验的用户名。
///
/// @return true 合法；false 非法。
bool IsValidUserName(const std::string& strName)
{
    if (strName.empty() || strName.size() > kMaxNameLength)
    {
        return false;
    }
    for (std::size_t i = 0; i < strName.size(); ++i)
    {
        const char ch = strName[i];
        const bool bValid =
            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!bValid)
        {
            return false;
        }
    }
    return true;
}

// ====================================================================
// 流程依赖
// ====================================================================

/// @brief 流程依赖：本模块执行器 + 数据访问模块接口。
///
/// 按值传给流程、按值捕获进回调：promise 链可能在「模块对象生命周期之外」settle，
/// 依赖按值持有（执行器 shared_ptr / 接口 ScopedInterfacePtr）保证流程安全 ——
/// 这也让流程函数成为不接触模块实例的纯函数，回调在哪个线程上都安全。
struct CFlowDeps
{
    std::shared_ptr<no::CAsyncExecutor> spExec;  ///< 本模块执行器。
    sc::ScopedInterfacePtr<IUserTable> spTable;  ///< 数据访问模块接口。

    /// @brief 依赖是否齐备。
    bool IsReady() const { return spExec != nullptr && spTable != nullptr; }
};

// ====================================================================
// 本模块内的处理器（纯业务逻辑，不接触模块实例）
//
// then 层**不用判断 upResult**：上游被拒绝时，框架直接跳过本层（失败即停）；
// 只有 finally（下面 StepAudit）才需要看它。需要处理拒绝请另挂 .Catch。
// ====================================================================

/// @brief 处理器：校验用户 id。
///
/// then 层：上游被拒绝时框架不会调用本层，无需判断 upResult。
///
/// @param spCtx 业务上下文。
///
/// @return 兑现；id 非法返回 kUserInvalidParam。
no::CPromiseResult StepValidateUserId(no::CPromiseResult /*upResult*/, const std::shared_ptr<CUserOpContext>& spCtx)
{
    spCtx->strTrace += "校验参数;";
    if (spCtx->nUserId == 0)
    {
        spCtx->strError = "用户 id 非法";
        return no::CPromiseResult::Reject(kUserInvalidParam);
    }
    return no::CPromiseResult::Resolve();
}

/// @brief 处理器：校验注册入参（用户名 / 邮箱 / 等级）。
///
/// then 层：无需判断 upResult（上游被拒绝时本层不会被调用）。
///
/// @param spCtx 业务上下文（recRequest 为入参）。
///
/// @return 兑现；入参非法返回 kUserInvalidParam（失败即停，后续层不再执行）。
no::CPromiseResult StepValidateRecord(no::CPromiseResult /*upResult*/, const std::shared_ptr<CUserOpContext>& spCtx)
{
    spCtx->strTrace += "校验参数;";
    if (!IsValidUserName(spCtx->recRequest.strName))
    {
        spCtx->strError = "用户名非法（1-" + std::to_string(kMaxNameLength) + " 个字母 / 数字 / 下划线 / 连字符）";
        return no::CPromiseResult::Reject(kUserInvalidParam);
    }
    if (spCtx->recRequest.strMail.find('@') == std::string::npos)
    {
        spCtx->strError = "邮箱格式非法";
        return no::CPromiseResult::Reject(kUserInvalidParam);
    }
    if (spCtx->recRequest.nLevel <= 0)
    {
        spCtx->strError = "用户等级非法";
        return no::CPromiseResult::Reject(kUserInvalidParam);
    }
    return no::CPromiseResult::Resolve();
}

/// @brief 处理器：用户不存在则本层拒绝。
///
/// then 层：无需判断 upResult（上游被拒绝时本层不会被调用）。
///
/// @param spCtx 业务上下文（读 bExists）。
///
/// @return 存在兑现；不存在返回 kUserNotFound（失败即停，后续层不执行）。
no::CPromiseResult StepRejectIfAbsent(no::CPromiseResult /*upResult*/, const std::shared_ptr<CUserOpContext>& spCtx)
{
    if (!spCtx->bExists)
    {
        spCtx->strError = "用户不存在";
        spCtx->strTrace += "拒绝(用户不存在);";
        return no::CPromiseResult::Reject(kUserNotFound);
    }
    return no::CPromiseResult::Resolve();
}

/// @brief 处理器：用户已存在则本层拒绝（注册流程的查重分支）。
///
/// then 层：无需判断 upResult（上游被拒绝时本层不会被调用）。
///
/// @param spCtx 业务上下文（读 bExists）。
///
/// @return 不存在兑现；已存在返回 kUserDuplicate。
no::CPromiseResult StepRejectIfExists(no::CPromiseResult /*upResult*/, const std::shared_ptr<CUserOpContext>& spCtx)
{
    if (spCtx->bExists)
    {
        spCtx->strError = "用户已存在";
        spCtx->strTrace += "拒绝(用户已存在);";
        return no::CPromiseResult::Reject(kUserDuplicate);
    }
    return no::CPromiseResult::Resolve();
}

/// @brief 处理器：组装更新请求（用库中当前版本 + 本次要改的名字）。
///
/// then 层：无需判断 upResult（上游被拒绝时本层不会被调用）。
///
/// @param spCtx 业务上下文（recResult 为库中当前行）。
///
/// @return 兑现（此时 recRequest 已带合法期望版本）。
no::CPromiseResult StepPrepareRename(no::CPromiseResult /*upResult*/, const std::shared_ptr<CUserOpContext>& spCtx)
{
    if (!spCtx->bExists)
    {
        spCtx->strError = "用户不存在";
        return no::CPromiseResult::Reject(kUserNotFound);
    }

    CUserRecord recRequest = spCtx->recResult;  // 沿用库中其余字段。
    recRequest.strName = spCtx->recRequest.strName;
    recRequest.nVersion = spCtx->recResult.nVersion;  // 期望版本 = 读到的版本（乐观锁）。
    spCtx->recRequest = recRequest;
    spCtx->strTrace += "组装更新(期望版本=" + std::to_string(recRequest.nVersion) + ");";
    return no::CPromiseResult::Resolve();
}

/// @brief 处理器（finally）：审计日志（无论成败都执行，结果原样透传）。
///
/// @param upResult 上一层结果（可能是拒绝）。
/// @param spCtx 业务上下文。
///
/// @return upResult（finally 忽略返回值，不改变链的走向）。
no::CPromiseResult StepAudit(no::CPromiseResult upResult, const std::shared_ptr<CUserOpContext>& spCtx)
{
    // finally 层：成败都执行，upResult 可能是拒绝（返回值被忽略，原样透传）。
    spCtx->strTrace += "审计;";
    common::log::CLogger& logger = common::log::CLogger::Instance();
    const std::string strResult = upResult.IsFulfilled()
                                      ? std::string("兑现")
                                      : ("拒绝(码=" + std::to_string(upResult.Code()) + " " + spCtx->strError + ")");
    logger.Info("[用户业务] 审计 " + spCtx->strAction + " 用户=" + std::to_string(spCtx->nUserId) +
                " 结果=" + strResult + " 轨迹=" + spCtx->strTrace);
    return upResult;
}

// ====================================================================
// 跨模块桥接（纯异步、零阻塞）：把数据访问模块的 promise 接进本流程
//
// 写法就是 JS 的 `new Promise((resolve, reject) => ...)`：
//   ① executor 里发起数据访问模块的异步调用（立即返回，不等待、不占线程）；
//   ② 在它的 OnSettled 回调里做「跨模块拒绝码 → 业务码」语义转换，
//      然后 resolve() 兑现 / reject(码) 拒绝本 promise；
//   ③ 本流程的后续 handler 由这个 settle 自然触发（ThenPromise 已把它接进链里）。
// ====================================================================

/// @brief 桥接：查询用户（数据访问模块）。
///
/// 语义转换：数据访问层的「记录不存在」对业务来说不是错误 → 归一化为兑现 + bExists=false；
/// 其他拒绝（含 kException）统一映射为业务码 kUserDbUnavailable。
///
/// @param deps 流程依赖（执行器 + 数据访问接口）。
/// @param spCtx 业务上下文（写入 bExists / recResult）。
///
/// @return 本流程的 promise（由数据访问模块的回调 settle）。
CUserPromise BridgeQueryUser(const CFlowDeps& deps, const std::shared_ptr<CUserOpContext>& spCtx)
{
    // executor 先赋给具名变量再用：长行不会被 clang-format 对齐撑开（见 docs/vscode-clangd-format.md）。
    CUserPromise::PromiseExecutor fnExecutor = [deps, spCtx](const ResolveFn& fnResolve, const RejectFn& fnReject)
    {
        spCtx->spDbOp->nUserId = spCtx->nUserId;
        spCtx->spDbOp->bFound = false;
        deps.spTable->QueryUserAsync(spCtx->spDbOp)
            .OnSettled([spCtx, fnResolve, fnReject](no::CPromiseResult result)
        {
            if (result.IsFulfilled())
            {
                spCtx->bExists = true;
                spCtx->recResult = spCtx->spDbOp->recResult;
                spCtx->strTrace += "查库命中;";
                fnResolve();
                return;
            }
            if (result.Code() == kDbRowNotFound)
            {
                spCtx->bExists = false;  // 语义转换：查询没查到不是错误。
                spCtx->strTrace += "查库未命中;";
                fnResolve();
                return;
            }
            spCtx->strError = "查询失败：数据访问码=" + std::to_string(result.Code());
            spCtx->strTrace += "失败(查询);";
            fnReject(kUserDbUnavailable);
        });
    };
    return CUserPromise::New(*deps.spExec, spCtx, fnExecutor, ASYNC_LOC);
}

/// @brief 桥接：插入用户（数据访问模块异步插入）。
///
/// @param deps 流程依赖。
/// @param spCtx 业务上下文（写入分配到的 id 与结果行）。
///
/// @return 本流程的 promise（由数据访问模块的回调 settle）。
CUserPromise BridgeInsertUser(const CFlowDeps& deps, const std::shared_ptr<CUserOpContext>& spCtx)
{
    CUserPromise::PromiseExecutor fnExecutor = [deps, spCtx](const ResolveFn& fnResolve, const RejectFn& fnReject)
    {
        spCtx->spDbOp->nUserId = spCtx->recRequest.nUserId;  // 0：由数据访问层自增分配。
        spCtx->spDbOp->recRequest = spCtx->recRequest;
        deps.spTable->InsertUserAsync(spCtx->spDbOp)
            .OnSettled([spCtx, fnResolve, fnReject](no::CPromiseResult result)
        {
            if (result.IsFulfilled())
            {
                spCtx->recResult = spCtx->spDbOp->recResult;
                spCtx->nUserId = spCtx->recResult.nUserId;
                spCtx->strTrace += "写库成功(id=" + std::to_string(spCtx->nUserId) + ");";
                fnResolve();
                return;
            }
            if (result.Code() == kDbDuplicateKey)
            {
                spCtx->strError = "用户已存在";
                spCtx->strTrace += "写库冲突;";
                fnReject(kUserDuplicate);
                return;
            }
            spCtx->strError = "插入失败：数据访问码=" + std::to_string(result.Code());
            spCtx->strTrace += "失败(插入);";
            fnReject(kUserDbUnavailable);
        });
    };
    return CUserPromise::New(*deps.spExec, spCtx, fnExecutor, ASYNC_LOC);
}

/// @brief 更新尝试（乐观锁冲突时在回调里重试 —— 回调驱动，不阻塞、不占线程）。
///
/// 每次尝试都会重新走数据访问模块的「读表 + 乐观锁更新」promise：版本不匹配时
/// 数据访问层回吐库中最新行，本层据此更新期望版本后立即再试一次；
/// 尝试次数用尽则以 kUserVersionConflict 拒绝。
///
/// @param deps 流程依赖。
/// @param spCtx 业务上下文（recRequest 为期望版本 + 新字段；nAttempt 记录尝试次数）。
/// @param fnResolve 本次桥接的兑现句柄。
/// @param fnReject 本次桥接的拒绝句柄。
/// @param nAttempt 本次是第几次尝试（从 1 开始）。
void UpdateUserAttempt(const CFlowDeps& deps, const std::shared_ptr<CUserOpContext>& spCtx, const ResolveFn& fnResolve,
                       const RejectFn& fnReject, int nAttempt)
{
    spCtx->nAttempt = nAttempt;
    spCtx->spDbOp->nUserId = spCtx->nUserId;
    spCtx->spDbOp->recRequest = spCtx->recRequest;
    deps.spTable->UpdateUserAsync(spCtx->spDbOp)
        .OnSettled([deps, spCtx, fnResolve, fnReject, nAttempt](no::CPromiseResult result)
    {
        if (result.IsFulfilled())
        {
            spCtx->recResult = spCtx->spDbOp->recResult;
            spCtx->strTrace += "改库成功(第" + std::to_string(nAttempt) + "次);";
            fnResolve();
            return;
        }

        if (result.Code() != kDbVersionConflict || nAttempt >= kMaxUpdateAttempts)
        {
            const bool bConflict = (result.Code() == kDbVersionConflict);
            spCtx->strError =
                bConflict ? "乐观锁冲突重试次数用尽" : ("更新失败：数据访问码=" + std::to_string(result.Code()));
            spCtx->strTrace += "失败(更新);";
            fnReject(bConflict ? kUserVersionConflict : kUserDbUnavailable);
            return;
        }

        // 版本冲突：用数据访问层回吐的最新行（含新版本号）重建请求，再试一次。
        CUserRecord recLatest = spCtx->spDbOp->recResult;
        recLatest.strName = spCtx->recRequest.strName;  // 保留本次要改的字段。
        spCtx->recRequest = recLatest;
        spCtx->strTrace += "版本冲突重试;";
        UpdateUserAttempt(deps, spCtx, fnResolve, fnReject, nAttempt + 1);
    });
}

/// @brief 桥接：更新用户（数据访问模块异步更新；含乐观锁冲突重试）。
///
/// @param deps 流程依赖。
/// @param spCtx 业务上下文。
///
/// @return 本流程的 promise（由数据访问模块的回调 settle）。
CUserPromise BridgeUpdateUser(const CFlowDeps& deps, const std::shared_ptr<CUserOpContext>& spCtx)
{
    CUserPromise::PromiseExecutor fnExecutor = [deps, spCtx](const ResolveFn& fnResolve, const RejectFn& fnReject)
    { UpdateUserAttempt(deps, spCtx, fnResolve, fnReject, 1); };
    return CUserPromise::New(*deps.spExec, spCtx, fnExecutor, ASYNC_LOC);
}

/// @brief 桥接：删除用户（数据访问模块异步删除）。
///
/// @param deps 流程依赖。
/// @param spCtx 业务上下文。
///
/// @return 本流程的 promise（由数据访问模块的回调 settle）。
CUserPromise BridgeDeleteUser(const CFlowDeps& deps, const std::shared_ptr<CUserOpContext>& spCtx)
{
    CUserPromise::PromiseExecutor fnExecutor = [deps, spCtx](const ResolveFn& fnResolve, const RejectFn& fnReject)
    {
        spCtx->spDbOp->nUserId = spCtx->nUserId;
        deps.spTable->DeleteUserAsync(spCtx->spDbOp)
            .OnSettled([spCtx, fnResolve, fnReject](no::CPromiseResult result)
        {
            if (result.IsFulfilled())
            {
                spCtx->strTrace += "删库成功;";
                fnResolve();
                return;
            }
            if (result.Code() == kDbRowNotFound)
            {
                spCtx->strError = "用户不存在";
                fnReject(kUserNotFound);
                return;
            }
            spCtx->strError = "删除失败：数据访问码=" + std::to_string(result.Code());
            spCtx->strTrace += "失败(删除);";
            fnReject(kUserDbUnavailable);
        });
    };
    return CUserPromise::New(*deps.spExec, spCtx, fnExecutor, ASYNC_LOC);
}

// ====================================================================
// 本模块内的异步函数与业务流程（纯异步：then / catch / finally + then-promise）
// ====================================================================

/// @brief 本模块内的异步函数：读用户（校验 → 跨模块查询）。
///
/// 被查询 / 改名 / 删除三个流程复用：同上下文类型，直接串接即可（非阻塞）。
///
/// @param deps 流程依赖。
/// @param spCtx 业务上下文（命中结果写入 bExists / recResult）。
///
/// @return 本流程的 promise。
CUserPromise LoadUserAsync(const CFlowDeps& deps, const std::shared_ptr<CUserOpContext>& spCtx)
{
    return deps.spExec->NewPromise(spCtx, &StepValidateUserId, ASYNC_LOC)
        .ThenPromise([deps](const std::shared_ptr<CUserOpContext>& spCtxSelf)
    { return BridgeQueryUser(deps, spCtxSelf); }, ASYNC_LOC);
}

/// @brief 流程：查询用户（读）。
///
/// 读用户 → 不存在即拒绝 → 审计收尾（finally）。
///
/// @param deps 流程依赖。
/// @param spCtx 业务上下文。
///
/// @return 本流程的 promise（结果数据在上下文中）。
CUserPromise BuildQueryFlow(const CFlowDeps& deps, const std::shared_ptr<CUserOpContext>& spCtx)
{
    return LoadUserAsync(deps, spCtx).Then(&StepRejectIfAbsent, ASYNC_LOC).Finally(&StepAudit, ASYNC_LOC);
}

/// @brief 流程：注册用户（写）。
///
/// 校验 → 读用户（查重）→ 查重命中即拒绝（后续落库层不执行）→ 跨模块插入 → 审计收尾。
///
/// @param deps 流程依赖。
/// @param spCtx 业务上下文（recRequest 为入参）。
///
/// @return 本流程的 promise（成功时 nUserId / recResult 为分配结果）。
CUserPromise BuildRegisterFlow(const CFlowDeps& deps, const std::shared_ptr<CUserOpContext>& spCtx)
{
    return deps.spExec->NewPromise(spCtx, &StepValidateRecord, ASYNC_LOC)
        .ThenPromise([deps](const std::shared_ptr<CUserOpContext>& spCtxSelf)
    { return BridgeQueryUser(deps, spCtxSelf); }, ASYNC_LOC)
        .Then(&StepRejectIfExists, ASYNC_LOC)  // 查重：已存在则拒绝（失败即停）
        .ThenPromise([deps](const std::shared_ptr<CUserOpContext>& spCtxSelf)
    { return BridgeInsertUser(deps, spCtxSelf); }, ASYNC_LOC)
        .Finally(&StepAudit, ASYNC_LOC);
}

/// @brief 流程：修改用户名（改）。
///
/// 读用户 → 不存在即拒绝 → 组装更新（带期望版本）→ 跨模块更新（冲突自动重试）→ 审计收尾。
///
/// @param deps 流程依赖。
/// @param spCtx 业务上下文（recRequest.strName 为新名字）。
///
/// @return 本流程的 promise（冲突重试次数见上下文的 nAttempt）。
CUserPromise BuildRenameFlow(const CFlowDeps& deps, const std::shared_ptr<CUserOpContext>& spCtx)
{
    return LoadUserAsync(deps, spCtx)
        .Then(&StepRejectIfAbsent, ASYNC_LOC)
        .Then(&StepPrepareRename, ASYNC_LOC)
        .ThenPromise([deps](const std::shared_ptr<CUserOpContext>& spCtxSelf)
    { return BridgeUpdateUser(deps, spCtxSelf); }, ASYNC_LOC)
        .Finally(&StepAudit, ASYNC_LOC);
}

/// @brief 流程：删除用户（删）。
///
/// 读用户 → 不存在即拒绝 → 跨模块删除 → 审计收尾。
///
/// @param deps 流程依赖。
/// @param spCtx 业务上下文。
///
/// @return 本流程的 promise。
CUserPromise BuildRemoveFlow(const CFlowDeps& deps, const std::shared_ptr<CUserOpContext>& spCtx)
{
    return LoadUserAsync(deps, spCtx)
        .Then(&StepRejectIfAbsent, ASYNC_LOC)
        .ThenPromise([deps](const std::shared_ptr<CUserOpContext>& spCtxSelf)
    { return BridgeDeleteUser(deps, spCtxSelf); }, ASYNC_LOC)
        .Finally(&StepAudit, ASYNC_LOC);
}

// ====================================================================
// 演示驱动：像外部调用方一样按接口调用本模块的异步函数
//
// 全回调驱动（零阻塞、不占线程）：每个场景在 OnSettled 回调里记录结果并启动下一个场景。
// 驱动持有接口引用（Self<IUserService>()）：回调期间业务模块存活，无需任何等待。
// ====================================================================

/// @brief 演示驱动（周期性跑一遍完整业务流程）。
class CDemoDriver : public std::enable_shared_from_this<CDemoDriver>
{
   public:
    /// @brief 创建演示驱动。
    ///
    /// @param spService 业务模块接口（自持引用：回调期间模块存活）。
    /// @param spTable 数据访问模块接口（场景⑧需要直接观察数据访问层的异常语义）。
    CDemoDriver(const sc::ScopedInterfacePtr<IUserService>& spService,
                const sc::ScopedInterfacePtr<IUserTable>& spTable)
        : m_spService(spService), m_spTable(spTable), m_nUserId(0), m_nRenamePending(0), m_strRenameSummary()
    {}

    /// @brief 启动演示（场景①）。
    void Run()
    {
        common::log::CLogger::Instance().Info(
            "=========== 用户业务演示开始（纯异步：promise / then / catch / finally，零阻塞）===========");
        RunRegister();
    }

   private:
    /// @brief 记录场景结果并决定是否继续。
    ///
    /// @param strScenario 场景名。
    /// @param result 本场景最终结果。
    /// @param spCtx 本场景上下文。
    void LogScenario(const std::string& strScenario, const no::CPromiseResult& result,
                     const std::shared_ptr<CUserOpContext>& spCtx)
    {
        const std::string strLine = "[" + strScenario + "] " + DescribeResult(result) +
                                    (spCtx->strError.empty() ? std::string() : (" 说明=" + spCtx->strError)) +
                                    " 轨迹=" + spCtx->strTrace;
        if (result.IsFulfilled())
        {
            common::log::CLogger::Instance().Info(strLine);
        }
        else
        {
            common::log::CLogger::Instance().Warn(strLine);
        }
    }

    /// @brief 场景①：注册用户（写）。
    void RunRegister()
    {
        CUserRecord recNew;
        recNew.strName = "carol";
        recNew.strMail = "carol@example.com";
        recNew.nLevel = 2;

        CUserPromise promise = m_spService->RegisterUserAsync(recNew);
        const std::shared_ptr<CUserOpContext> spCtx = promise.GetContext();
        std::shared_ptr<CDemoDriver> spSelf = shared_from_this();
        promise.OnSettled([spSelf, spCtx](no::CPromiseResult result)
        {
            spSelf->m_nUserId = spCtx->nUserId;
            spSelf->LogScenario("演示① 注册用户", result, spCtx);
            if (spCtx->nUserId == 0)
            {
                common::log::CLogger::Instance().Warn("[演示] 注册未成功，跳过依赖该用户的场景");
                spSelf->RunInvalidParam();
                return;
            }
            spSelf->RunDuplicateRegister();
        });
    }

    /// @brief 场景②：重复注册同一用户（查重命中 → 业务拒绝码 kUserDuplicate）。
    void RunDuplicateRegister()
    {
        CUserRecord recDup;
        recDup.nUserId = m_nUserId;
        recDup.strName = "carol2";
        recDup.strMail = "carol2@example.com";

        CUserPromise promise = m_spService->RegisterUserAsync(recDup);
        const std::shared_ptr<CUserOpContext> spCtx = promise.GetContext();
        std::shared_ptr<CDemoDriver> spSelf = shared_from_this();
        promise.OnSettled([spSelf, spCtx](no::CPromiseResult result)
        {
            spSelf->LogScenario("演示② 重复注册（查重命中）", result, spCtx);
            spSelf->RunQuery();
        });
    }

    /// @brief 场景③：查询用户（读）。
    void RunQuery()
    {
        CUserPromise promise = m_spService->QueryUserAsync(m_nUserId);
        const std::shared_ptr<CUserOpContext> spCtx = promise.GetContext();
        std::shared_ptr<CDemoDriver> spSelf = shared_from_this();
        promise.OnSettled([spSelf, spCtx](no::CPromiseResult result)
        {
            spSelf->LogScenario(
                "演示③ 查询用户 名字=" + spCtx->recResult.strName + " 等级=" + std::to_string(spCtx->recResult.nLevel),
                result, spCtx);
            spSelf->RunRenameConcurrent();
        });
    }

    /// @brief 场景④：并发改名 —— 两条链同时改同一行，乐观锁冲突由业务层在回调里自动重试。
    void RunRenameConcurrent()
    {
        m_nRenamePending.store(2, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_strRenameSummary.clear();
        }
        StartRename("A", "carol-a");
        StartRename("B", "carol-b");
    }

    /// @brief 发起一路改名（两路并发，最后由最后完成者收口）。
    ///
    /// @param strTag 分支标记（A / B）。
    /// @param strNewName 本分支要改成的名字。
    void StartRename(const std::string& strTag, const std::string& strNewName)
    {
        CUserPromise promise = m_spService->RenameUserAsync(m_nUserId, strNewName);
        const std::shared_ptr<CUserOpContext> spCtx = promise.GetContext();
        std::shared_ptr<CDemoDriver> spSelf = shared_from_this();
        promise.OnSettled([spSelf, spCtx, strTag](no::CPromiseResult result)
        {
            const std::string strLine = strTag + "=" + DescribeResult(result) +
                                        " 尝试=" + std::to_string(spCtx->nAttempt) + " 轨迹=" + spCtx->strTrace;
            {
                // 两路回调可能在不同线程 → 汇总数据加锁。
                std::lock_guard<std::mutex> lock(spSelf->m_mutex);
                if (!spSelf->m_strRenameSummary.empty())
                {
                    spSelf->m_strRenameSummary += " | ";
                }
                spSelf->m_strRenameSummary += strLine;
            }
            if (spSelf->m_nRenamePending.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                std::string strSummary;
                {
                    std::lock_guard<std::mutex> lock(spSelf->m_mutex);
                    strSummary = spSelf->m_strRenameSummary;
                }
                common::log::CLogger::Instance().Info("[演示④] 并发改名（乐观锁冲突自动重试）" + strSummary);
                spSelf->RunRemove();
            }
        });
    }

    /// @brief 场景⑤：删除用户（删）。
    void RunRemove()
    {
        CUserPromise promise = m_spService->RemoveUserAsync(m_nUserId);
        const std::shared_ptr<CUserOpContext> spCtx = promise.GetContext();
        std::shared_ptr<CDemoDriver> spSelf = shared_from_this();
        promise.OnSettled([spSelf, spCtx](no::CPromiseResult result)
        {
            spSelf->LogScenario("演示⑤ 删除用户", result, spCtx);
            spSelf->RunRemoveAgain();
        });
    }

    /// @brief 场景⑥：重复删除（数据访问层未命中 → 业务拒绝码 kUserNotFound）。
    void RunRemoveAgain()
    {
        CUserPromise promise = m_spService->RemoveUserAsync(m_nUserId);
        const std::shared_ptr<CUserOpContext> spCtx = promise.GetContext();
        std::shared_ptr<CDemoDriver> spSelf = shared_from_this();
        promise.OnSettled([spSelf, spCtx](no::CPromiseResult result)
        {
            spSelf->LogScenario("演示⑥ 重复删除（已不存在）", result, spCtx);
            spSelf->RunInvalidParam();
        });
    }

    /// @brief 场景⑦：非法入参（本模块校验层直接拒绝，后续层不执行）。
    void RunInvalidParam()
    {
        CUserRecord recBad;
        recBad.strName = "";
        recBad.strMail = "bad@example.com";

        CUserPromise promise = m_spService->RegisterUserAsync(recBad);
        const std::shared_ptr<CUserOpContext> spCtx = promise.GetContext();
        std::shared_ptr<CDemoDriver> spSelf = shared_from_this();
        promise.OnSettled([spSelf, spCtx](no::CPromiseResult result)
        {
            spSelf->LogScenario("演示⑦ 非法入参", result, spCtx);
            spSelf->RunDbError();
        });
    }

    /// @brief 场景⑧：数据访问模块层内异常（模拟驱动故障）。
    ///
    /// 处理器内抛出异常 → 框架转为 kException（不向调用方抛出），
    /// 且数据访问模块的 finally 层（放连接）仍然执行。
    void RunDbError()
    {
        std::shared_ptr<CUserTableOp> spDbOp(new CUserTableOp());
        spDbOp->nUserId = m_nUserId;
        spDbOp->bSimulateDbError = true;
        std::shared_ptr<CDemoDriver> spSelf = shared_from_this();
        m_spTable->QueryUserAsync(spDbOp).OnSettled([spSelf, spDbOp](no::CPromiseResult result)
        {
            common::log::CLogger::Instance().Warn("[演示⑧] 数据访问层内异常 " + DescribeResult(result) +
                                                  "（kException=" + std::to_string(no::kException) +
                                                  "）轨迹=" + spDbOp->strTrace);
            common::log::CLogger::Instance().Info(
                "=========== 用户业务演示结束（全流程回调驱动，未阻塞任何线程）===========");
        });
    }

    sc::ScopedInterfacePtr<IUserService> m_spService;  ///< 业务模块接口（自持引用）。
    sc::ScopedInterfacePtr<IUserTable> m_spTable;      ///< 数据访问模块接口。
    std::uint64_t m_nUserId;                           ///< 演示用户 id（场景①分配）。
    std::atomic<int> m_nRenamePending;                 ///< 并发改名未完成的分支数。
    std::mutex m_mutex;                                ///< 保护并发场景的汇总数据。
    std::string m_strRenameSummary;                    ///< 并发改名各分支结果汇总。
};

}  // namespace

/// @brief 接口查询实现（自定义接口使用显式接口标识条目）。
SC_BEGIN_INTERFACE_MAP(CExampleAsyncModule, sc::CModule)
SC_INTERFACE_ENTRY_EX(IUserService, IID_IUserService())
SC_END_INTERFACE_MAP(CExampleAsyncModule, sc::CModule)

/// @brief 创建用户资料业务模块。
///
/// @param nIntervalMs 周期演示间隔（毫秒，小于 100 按 100 处理）。
CExampleAsyncModule::CExampleAsyncModule(std::int64_t nIntervalMs)
    : sc::CModule("user-service"), m_nIntervalMs(nIntervalMs), m_tTimerId(common::timer::kInvalidTimerId)
{
    // 依赖数据访问模块接口：生命周期拓扑排序保证其先初始化 / 启动。
    AddDependency(IID_IUserTable());
    if (m_nIntervalMs < 100)
    {
        m_nIntervalMs = 100;
    }
}

/// @brief 销毁业务模块。
CExampleAsyncModule::~CExampleAsyncModule()
{
    Stop();
}

/// @brief 解析 ITimer 与数据访问（IUserTable）接口。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true 定时器与数据访问接口就绪；false 缺失。
bool CExampleAsyncModule::Initialize(const sc::CResolveContext& ctx)
{
    m_pTimer.Reset(ctx.Resolve<sc::ITimer>());
    m_pUserTable.Reset(ctx.Resolve<IUserTable>(IID_IUserTable()));
    return m_pTimer != nullptr && m_pUserTable != nullptr;
}

/// @brief 启动自建执行器并注册周期演示定时器。
///
/// 线程数取 2 即可：本模块的异步流程**从不阻塞等待**（跨模块调用由回调驱动），
/// 线程只用于跑本模块的 handler。
///
/// @return true 启动成功；false 执行器或依赖缺失。
bool CExampleAsyncModule::Start()
{
    if (m_pTimer == nullptr || m_pUserTable == nullptr)
    {
        return false;
    }

    // promise 是模板（上下文类型固定），无法放进 IAsyncExecutor 虚接口，故自持执行器；
    // 用 shared_ptr 持有并按值传给流程：流程/回调可能晚于模块停止，
    // 执行器对象按引用计数存活（停止后新投递以 kStopped 被拒绝）。
    m_spExecutor.reset(new common::async::CAsyncExecutor(2));
    if (!m_spExecutor->Start())
    {
        m_spExecutor.reset();
        return false;
    }

    // 周期演示：弱引用守卫，模块停止 / 销毁后回调自动跳过。
    m_tTimerId = sc::AddGuardedPeriodicTimer(m_pTimer.Get(), m_nIntervalMs, WeakSelf<CExampleAsyncModule>(),
                                             [](const sc::ScopedInterfacePtr<CExampleAsyncModule>& sp)
    { sp->ScheduleExample(); });
    return true;
}

/// @brief 取消定时器并停止执行器。
///
/// CAsyncExecutor::Stop 等待已投递任务完成（优雅关闭）；已在途的跨模块回调
/// 只写上下文与 settle promise（不接触本模块对象），因此不会被 Stop 打断。
void CExampleAsyncModule::Stop()
{
    if (m_tTimerId != common::timer::kInvalidTimerId)
    {
        if (m_pTimer != nullptr)
        {
            m_pTimer->Cancel(m_tTimerId);
        }
        m_tTimerId = common::timer::kInvalidTimerId;
    }
    if (m_spExecutor != nullptr)
    {
        m_spExecutor->Stop();
    }
}

/// @brief 停止并释放接口引用。
void CExampleAsyncModule::Shutdown()
{
    Stop();
    m_spExecutor.reset();
    m_pUserTable.Reset();
    m_pTimer.Reset();
}

/// @brief 状态报告。
///
/// @return 模块名 + 演示周期 + 数据访问接口可用性。
std::string CExampleAsyncModule::GetStatus() const
{
    return "user-service 周期=" + std::to_string(m_nIntervalMs) +
           "ms 数据访问=" + std::string(m_pUserTable != nullptr ? "就绪" : "缺失");
}

/// @brief 创建业务操作上下文。
///
/// @param strAction 操作名（审计 / 日志用）。
///
/// @return 上下文（数据访问操作上下文已在构造中创建）。
std::shared_ptr<CUserOpContext> CExampleAsyncModule::MakeContext(const std::string& strAction)
{
    std::shared_ptr<CUserOpContext> spCtx(new CUserOpContext());
    spCtx->strAction = strAction;
    return spCtx;
}

/// @brief 组装流程依赖（按值传给流程：执行器 shared_ptr + 数据访问接口）。
///
/// @return 流程依赖。
static CFlowDeps MakeFlowDeps(const std::shared_ptr<common::async::CAsyncExecutor>& spExec,
                              const sc::ScopedInterfacePtr<IUserTable>& spTable)
{
    CFlowDeps deps;
    deps.spExec = spExec;
    deps.spTable = spTable;
    return deps;
}

/// @brief 依赖缺失时构造「已拒绝」的 promise（调用方回调照常触发，不会静默丢失）。
///
/// @param deps 流程依赖（执行器须非空）。
/// @param spCtx 业务上下文。
///
/// @return 立即以 kUserDbUnavailable 被拒绝的 promise。
static CUserPromise MakeRejectedPromise(const CFlowDeps& deps, const std::shared_ptr<CUserOpContext>& spCtx)
{
    CUserPromise::PromiseExecutor fnExecutor = [spCtx](const ResolveFn& /*fnResolve*/, const RejectFn& fnReject)
    {
        spCtx->strError = "数据访问模块不可用";
        fnReject(kUserDbUnavailable);
    };
    return CUserPromise::New(*deps.spExec, spCtx, fnExecutor, ASYNC_LOC);
}

/// @brief 异步查询用户信息（读）。
///
/// @param nUserId 目标用户 id。
///
/// @return promise 句柄（立即返回；结果数据在 GetContext() 中）。
common::async::CPromise<CUserOpContext> CExampleAsyncModule::QueryUserAsync(std::uint64_t nUserId)
{
    std::shared_ptr<CUserOpContext> spCtx = MakeContext("查询用户");
    spCtx->nUserId = nUserId;
    const CFlowDeps deps = MakeFlowDeps(m_spExecutor, m_pUserTable);
    if (deps.spExec == nullptr)
    {
        return common::async::CPromise<CUserOpContext>();  // 未启动：无效 promise（无法调度）。
    }
    if (deps.spTable == nullptr)
    {
        return MakeRejectedPromise(deps, spCtx);
    }
    return BuildQueryFlow(deps, spCtx);
}

/// @brief 异步注册用户（写）。
///
/// @param recRequest 待注册记录（nUserId 为 0 时由数据访问层自增分配）。
///
/// @return promise 句柄（注册成功时 nUserId / recResult 为分配结果）。
common::async::CPromise<CUserOpContext> CExampleAsyncModule::RegisterUserAsync(const CUserRecord& recRequest)
{
    std::shared_ptr<CUserOpContext> spCtx = MakeContext("注册用户");
    spCtx->recRequest = recRequest;
    spCtx->nUserId = recRequest.nUserId;  // 指定了 id 时先查重；为 0（自增分配）时查库自然未命中。
    const CFlowDeps deps = MakeFlowDeps(m_spExecutor, m_pUserTable);
    if (deps.spExec == nullptr)
    {
        return common::async::CPromise<CUserOpContext>();
    }
    if (deps.spTable == nullptr)
    {
        return MakeRejectedPromise(deps, spCtx);
    }
    return BuildRegisterFlow(deps, spCtx);
}

/// @brief 异步修改用户名（改）。
///
/// @param nUserId 目标用户 id。
/// @param strNewName 新用户名。
///
/// @return promise 句柄（冲突重试次数见上下文的 nAttempt）。
common::async::CPromise<CUserOpContext> CExampleAsyncModule::RenameUserAsync(std::uint64_t nUserId,
                                                                             const std::string& strNewName)
{
    std::shared_ptr<CUserOpContext> spCtx = MakeContext("修改用户名");
    spCtx->nUserId = nUserId;
    spCtx->recRequest.strName = strNewName;  // 只带新名字：其余字段由 StepPrepareRename 从库中补齐。
    const CFlowDeps deps = MakeFlowDeps(m_spExecutor, m_pUserTable);
    if (deps.spExec == nullptr)
    {
        return common::async::CPromise<CUserOpContext>();
    }
    if (deps.spTable == nullptr)
    {
        return MakeRejectedPromise(deps, spCtx);
    }
    return BuildRenameFlow(deps, spCtx);
}

/// @brief 异步删除用户（删）。
///
/// @param nUserId 目标用户 id。
///
/// @return promise 句柄。
common::async::CPromise<CUserOpContext> CExampleAsyncModule::RemoveUserAsync(std::uint64_t nUserId)
{
    std::shared_ptr<CUserOpContext> spCtx = MakeContext("删除用户");
    spCtx->nUserId = nUserId;
    const CFlowDeps deps = MakeFlowDeps(m_spExecutor, m_pUserTable);
    if (deps.spExec == nullptr)
    {
        return common::async::CPromise<CUserOpContext>();
    }
    if (deps.spTable == nullptr)
    {
        return MakeRejectedPromise(deps, spCtx);
    }
    return BuildRemoveFlow(deps, spCtx);
}

/// @brief 定时器回调：投递一轮演示。
///
/// 演示驱动全程回调驱动（不阻塞任何线程），这里只把它投递到本模块执行器，
/// 避免定时器线程上跑业务代码。
void CExampleAsyncModule::ScheduleExample()
{
    if (m_spExecutor == nullptr)
    {
        return;
    }
    // 驱动持有接口自持引用（Self<IUserService>()）：回调期间模块存活。
    std::shared_ptr<CDemoDriver> spDriver(new CDemoDriver(Self<IUserService>(), m_pUserTable));
    if (!m_spExecutor->Post([spDriver]() { spDriver->Run(); }))
    {
        common::log::CLogger::Instance().Warn("[演示] 演示任务投递失败（执行器已停止）");
    }
}

}  // namespace serverexample
