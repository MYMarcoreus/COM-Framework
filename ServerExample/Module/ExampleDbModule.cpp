#include "Module/ExampleDbModule.h"

#include <chrono>
#include <stdexcept>
#include <thread>

#include "Assert.h"
#include "Log/Logger.h"
#include "Module/ResolveContext.h"

namespace serverexample {

namespace {

/// @brief 种子数据：启动时装载到表中（模拟数据库里已有的数据行）。
const std::uint64_t kSeedUserIdAlice = 1001;
const std::uint64_t kSeedUserIdBob = 1002;

/// @brief 自增主键起始值（避开种子数据）。
const std::uint64_t kFirstAutoId = 2001;

/// @brief 模拟数据库 IO 延迟（毫秒；0 表示不休眠）。
void SimulateDbIo(int nLatencyMs)
{
    if (nLatencyMs > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(nLatencyMs));
    }
}

}  // namespace

/// @brief 接口查询实现（自定义接口使用显式接口标识条目）。
SC_BEGIN_INTERFACE_MAP(CExampleDbModule, sc::CModule)
SC_INTERFACE_ENTRY_EX(IUserTable, IID_IUserTable())
SC_END_INTERFACE_MAP(CExampleDbModule, sc::CModule)

/// @brief 创建模拟数据库模块。
///
/// @param nLatencyMs 每次表操作的模拟 IO 延迟（毫秒，负数按 0 处理）。
/// @param nThreadCount 执行器线程数（同时能跑几个读；小于 1 按 1 处理）。
CExampleDbModule::CExampleDbModule(int nLatencyMs, int nThreadCount)
    : sc::CModule("example-db"), m_nRows(0), m_nNextId(kFirstAutoId), m_nLatencyMs(nLatencyMs), m_nThreadCount(nThreadCount)
{
    if (m_nLatencyMs < 0)
    {
        m_nLatencyMs = 0;
    }
    if (m_nThreadCount < 1)
    {
        m_nThreadCount = 1;
    }
}

/// @brief 销毁模拟数据库模块。
CExampleDbModule::~CExampleDbModule()
{
    Stop();
}

/// @brief 初始化（本模块无外部依赖）。
///
/// @param ctx 初始化上下文（依赖注入；本模块未声明依赖，不使用）。
///
/// @return true。
bool CExampleDbModule::Initialize(const sc::CResolveContext& ctx)
{
    (void)ctx;
    return true;
}

/// @brief 装载种子数据并启动自建执行器。
///
/// @return true 启动成功；false 执行器启动失败。
bool CExampleDbModule::Start()
{
    // ① 装载种子数据（模拟数据库里已有数据）。
    //    此时执行器还没启动 → 没有任何任务在跑，直接写表安全。
    m_mapRows.clear();

    CUserRecord recAlice;
    recAlice.nUserId = kSeedUserIdAlice;
    recAlice.strName = "alice";
    recAlice.strMail = "alice@example.com";
    recAlice.nLevel = 3;
    recAlice.nVersion = 1;
    m_mapRows[recAlice.nUserId] = recAlice;

    CUserRecord recBob;
    recBob.nUserId = kSeedUserIdBob;
    recBob.strName = "bob";
    recBob.strMail = "bob@example.com";
    recBob.nLevel = 1;
    recBob.nVersion = 1;
    m_mapRows[recBob.nUserId] = recBob;

    m_nRows.store(m_mapRows.size());

    // ② 自建执行器：promise 是模板（上下文类型固定为 CUserTableOp），无法放进
    //    IAsyncExecutor 虚接口，因此本模块自持具体执行器；本模块的层不占用调用方线程。
    //    读写门挂在执行器上：本模块的线程数 = 「同时能跑几个读」的上限（写始终独占）。
    m_pExecutor.reset(new common::async::CAsyncExecutor(m_nThreadCount));
    if (!m_pExecutor->Start())
    {
        m_pExecutor.reset();
        return false;
    }

    common::log::CLogger::Instance().Info("[数据访问] 模拟数据库已启动（种子数据 " + std::to_string(m_nRows.load()) +
                                          " 行，单次 IO 延迟 " + std::to_string(m_nLatencyMs) + "ms，执行器 " +
                                          std::to_string(m_nThreadCount) + " 线程：读可并发 / 写独占，表不加锁）");
    return true;
}

/// @brief 停止执行器。
///
/// CAsyncExecutor::Stop 等待已投递任务完成（优雅关闭），因此本模块的层不会在
/// Stop 返回后继续访问表数据。
void CExampleDbModule::Stop()
{
    if (m_pExecutor != nullptr)
    {
        m_pExecutor->Stop();
        m_pExecutor.reset();
    }
}

/// @brief 停止并清空表数据。
///
/// Stop 等待在途任务结束 → 此时没有任何读链 / 写链在跑，直接清表安全。
void CExampleDbModule::Shutdown()
{
    Stop();
    m_mapRows.clear();
    m_nRows.store(0);
}

/// @brief 状态报告。
///
/// @return 模块名 + 当前表行数。
///
/// @note 本函数「不受读写门保护」（它不在执行器任务里），所以只读原子行数、不碰表 ——
///       表数据只有「过门的读链 / 写链」能访问（读任务只读、写任务独占）。
std::string CExampleDbModule::GetStatus() const
{
    return "example-db 行数=" + std::to_string(m_nRows.load());
}

/// @brief 异步查询用户：读表 → 放连接（finally：失败也执行）。
///
/// 读链（`TaskKind::kRead`）：可与其他查询并发进入本模块（表只读）—— 模拟 IO 的等待互相重叠。
///
/// @param spOp 操作上下文（调用方提供，命中行写入 recResult）。
///
/// @return promise 句柄（未命中时最终以 CDbError(kRowNotFound) 被拒绝）。
common::async::CPromise<CUserTableOp> CExampleDbModule::QueryUserAsync(const std::shared_ptr<CUserTableOp>& spOp)
{
    // 契约：执行器在 Start() 中创建（失败即模块不可用），操作上下文由调用方提供且非空。
    ASSERT_MSG(m_pExecutor != nullptr, "模块未启动：没有执行器可调度，不应调用本接口");
    ASSERT_MSG(spOp != nullptr, "接口契约：操作上下文必须非空");
    return LoadRowAsync(spOp, common::async::TaskKind::kRead).Finally(BindResult(&CExampleDbModule::StepReleaseConn), common::async::TaskKind::kRead, ASYNC_LOC);
}

/// @brief 异步插入用户：「复用本模块内的读表异步函数」（查重）→ 写表 → 放连接。
///
/// 读表未命中（CDbError(kRowNotFound)）由 catch 归一化为兑现，因此查重层 / 写表层照常执行；
/// 其他拒绝（含框架侧失败）继续透传，写表层不执行。
///
/// @param spOp 操作上下文（recRequest 为待插入行；nUserId 为 0 时自增分配）。
///
/// @return promise 句柄（主键冲突时最终以 CDbError(kDuplicateKey) 被拒绝）。
common::async::CPromise<CUserTableOp> CExampleDbModule::InsertUserAsync(const std::shared_ptr<CUserTableOp>& spOp)
{
    ASSERT_MSG(m_pExecutor != nullptr, "模块未启动：没有执行器可调度，不应调用本接口");
    ASSERT_MSG(spOp != nullptr, "接口契约：操作上下文必须非空");
    // 写链（默认类别）：独占进入 —— 「读表（查重）→ 写表」整段不被其他任务插队。
    return LoadRowAsync(spOp, common::async::TaskKind::kWrite)                // 本模块内的异步函数（读表）
        .Catch(BindResult(&CExampleDbModule::StepAcceptNotFound), common::async::TaskKind::kWrite, ASYNC_LOC)  // 「不存在」归一化为兑现
        .Then(BindThen(&CExampleDbModule::StepRejectIfExists), common::async::TaskKind::kWrite, ASYNC_LOC)     // 查重
        .Then(BindThen(&CExampleDbModule::StepInsertRow), common::async::TaskKind::kWrite, ASYNC_LOC)          // 写表
        .Finally(BindResult(&CExampleDbModule::StepReleaseConn), common::async::TaskKind::kWrite, ASYNC_LOC);  // 收尾：放连接
}

/// @brief 异步更新用户：「复用本模块内的读表异步函数」 → 乐观锁写表 → 放连接。
///
/// @param spOp 操作上下文（recRequest 为期望版本 + 新字段）。
///
/// @return promise 句柄（版本冲突时最终以 CDbError(kVersionConflict) 被拒绝，recResult 为库中最新行）。
common::async::CPromise<CUserTableOp> CExampleDbModule::UpdateUserAsync(const std::shared_ptr<CUserTableOp>& spOp)
{
    ASSERT_MSG(m_pExecutor != nullptr, "模块未启动：没有执行器可调度，不应调用本接口");
    ASSERT_MSG(spOp != nullptr, "接口契约：操作上下文必须非空");
    // 写链（默认类别）：独占进入 —— 「读行（拿版本）→ 比对 → 写回」整段不被插队。
    return LoadRowAsync(spOp, common::async::TaskKind::kWrite)
        .Catch(BindResult(&CExampleDbModule::StepAcceptNotFound), common::async::TaskKind::kWrite, ASYNC_LOC)
        .Then(BindThen(&CExampleDbModule::StepApplyUpdate), common::async::TaskKind::kWrite, ASYNC_LOC)
        .Finally(BindResult(&CExampleDbModule::StepReleaseConn), common::async::TaskKind::kWrite, ASYNC_LOC);
}

/// @brief 异步删除用户：「复用本模块内的读表异步函数」（确认存在）→ 删行 → 放连接。
///
/// @param spOp 操作上下文（nUserId 为目标用户）。
///
/// @return promise 句柄（未命中时最终以 CDbError(kRowNotFound) 被拒绝）。
common::async::CPromise<CUserTableOp> CExampleDbModule::DeleteUserAsync(const std::shared_ptr<CUserTableOp>& spOp)
{
    ASSERT_MSG(m_pExecutor != nullptr, "模块未启动：没有执行器可调度，不应调用本接口");
    ASSERT_MSG(spOp != nullptr, "接口契约：操作上下文必须非空");
    // 写链（默认类别）：独占进入。
    return LoadRowAsync(spOp, common::async::TaskKind::kWrite)
        .Catch(BindResult(&CExampleDbModule::StepAcceptNotFound), common::async::TaskKind::kWrite, ASYNC_LOC)
        .Then(BindThen(&CExampleDbModule::StepEraseRow), common::async::TaskKind::kWrite, ASYNC_LOC)
        .Finally(BindResult(&CExampleDbModule::StepReleaseConn), common::async::TaskKind::kWrite, ASYNC_LOC);
}

/// @brief 本模块内的异步函数：读表（取连接 → 读行）。
///
/// 被查询 / 插入 / 更新 / 删除四个对外异步函数复用：同上下文类型，直接追加 handler 即可，
/// 不需要嵌套起 promise，也不会阻塞任何线程。
///
/// 类别由调用方给：查询流程传 `kRead`（可并发），写流程传 `kWrite`（独占）——
/// 后者使「读到的行不会被别人改掉，写回时也不会有人插队」，乐观锁在模块内不再自相冲突。
///
/// @param spOp 操作上下文（命中行写入 recResult，并置 bFound）。
/// @param eKind 本流程的读写类别（决定能否与其它读并发）。
///
/// @return promise 句柄（未命中时最终以 CDbError(kRowNotFound) 被拒绝）。
common::async::CPromise<CUserTableOp> CExampleDbModule::LoadRowAsync(
    const std::shared_ptr<CUserTableOp>& spOp, common::async::TaskKind eKind)
{
    return m_pExecutor->NewPromise(spOp, BindThen(&CExampleDbModule::StepAcquireConn), eKind, ASYNC_LOC)
        .Then(BindThen(&CExampleDbModule::StepLoadRow), eKind, ASYNC_LOC);
}

/// @brief 处理器（then）：取连接 + 模拟数据库 IO 延迟。
///
/// then 层：上游被拒绝时框架「不会调用本层」（失败即停），所以处理器只接上下文；
/// 只有 catch（StepAcceptNotFound）与 finally（StepReleaseConn）才拿得到上游结果。
///
/// @param spOp 操作上下文（追加轨迹）。
///
/// @return 兑现。
common::async::CPromiseResult CExampleDbModule::StepAcquireConn(const std::shared_ptr<CUserTableOp>& spOp)
{
    spOp->strTrace += "取连接;";
    SimulateDbIo(m_nLatencyMs);
    return common::async::CPromiseResult::Resolve();
}

/// @brief 处理器（then）：读表。
///
/// @param spOp 操作上下文（命中写 recResult / bFound；演示开关触发驱动异常）。
///
/// @return 命中兑现；未命中返回 CDbError(kRowNotFound)；驱动异常抛出（框架原样收口为拒绝）。
common::async::CPromiseResult CExampleDbModule::StepLoadRow(const std::shared_ptr<CUserTableOp>& spOp)
{
    SimulateDbIo(m_nLatencyMs);

    if (spOp->bSimulateDbError)
    {
        spOp->strTrace += "驱动异常;";
        throw std::runtime_error("模拟数据库驱动异常");  // 框架捕获 → 本层以异常原样被拒绝。
    }

    // 读链可并发：本层只读表（const_iterator）、不改模块状态 —— 与「读任务只读」的规约一致；
    // 写链是独占的，不会有人在读的同时改表，因此不需要锁。
    CUserRecord recRow;
    bool bFound = false;
    std::map<std::uint64_t, CUserRecord>::const_iterator it = m_mapRows.find(spOp->nUserId);
    if (it != m_mapRows.end())
    {
        recRow = it->second;
        bFound = true;
    }

    spOp->bFound = bFound;
    if (!bFound)
    {
        spOp->strTrace += "读表未命中(id=" + std::to_string(spOp->nUserId) + ");";
        return common::async::CPromiseResult::Reject(
            CDbError(CDbError::kRowNotFound, "记录不存在(id=" + std::to_string(spOp->nUserId) + ")"));
    }

    spOp->recResult = recRow;
    spOp->strTrace += "读表命中;";
    return common::async::CPromiseResult::Resolve();
}

/// @brief 处理器（catch）：把「记录不存在」归一化为兑现，供写入流程继续执行。
///
/// @param upResult 上一层结果（可能是 CDbError(kRowNotFound) 的拒绝）。
/// @param spOp 操作上下文（追加轨迹）。
///
/// @return 上一层为 `CDbError(kRowNotFound)` 时返回 Resolve()（吞掉该拒绝）；否则原样透传。
common::async::CPromiseResult CExampleDbModule::StepAcceptNotFound(
    common::async::CPromiseResult upResult, const std::shared_ptr<CUserTableOp>& spOp)
{
    // catch 层：只在被拒绝时执行，upResult 必定是拒绝 —— 只有这里才必须看它。
    CDbError::EKind eKind = CDbError::kRowNotFound;
    std::string strWhat;
    if (TryGetDbError(upResult, eKind, strWhat) && eKind == CDbError::kRowNotFound)
    {
        spOp->strTrace += "归一化(未命中→兑现);";
        return common::async::CPromiseResult::Resolve();
    }
    return upResult;  // 其他拒绝（业务 / 框架侧失败）继续透传：后续 Then 层不执行。
}

/// @brief 处理器（then）：查重（已存在则本层拒绝）。
///
/// @param spOp 操作上下文（读 bFound）。
///
/// @return 不存在兑现；存在返回 `CDbError(kDuplicateKey)`。
common::async::CPromiseResult CExampleDbModule::StepRejectIfExists(const std::shared_ptr<CUserTableOp>& spOp)
{
    if (spOp->bFound)
    {
        spOp->strTrace += "主键冲突;";
        return common::async::CPromiseResult::Reject(CDbError(CDbError::kDuplicateKey, "主键冲突（用户已存在）"));
    }
    return common::async::CPromiseResult::Resolve();
}

/// @brief 处理器（then）：写表（插入行，必要时分配自增主键）。
///
/// @param spOp 操作上下文（写入 recResult / nUserId）。
///
/// @return 兑现；主键冲突返回 CDbError(kDuplicateKey)。
common::async::CPromiseResult CExampleDbModule::StepInsertRow(const std::shared_ptr<CUserTableOp>& spOp)
{
    SimulateDbIo(m_nLatencyMs);

    // 写链独占：本层跑在独占区里（没有并发的读 / 写）→ 不需要锁。
    std::uint64_t nUserId = spOp->recRequest.nUserId;
    if (nUserId == 0)
    {
        nUserId = m_nNextId++;  // 自增分配主键（模拟数据库自增列）—— 独占区里改，不会竞态。
    }
    if (m_mapRows.find(nUserId) != m_mapRows.end())
    {
        spOp->strTrace += "主键冲突;";
        // 双保险：本流程的查重层已经查过（同在独占区里，模块内不会再插队）；
        // 这里拦的是「模块之外」的改动（另一个进程 / 直连数据库的写入）。
        return common::async::CPromiseResult::Reject(CDbError(CDbError::kDuplicateKey, "主键冲突（并发插入）"));
    }

    CUserRecord recRow = spOp->recRequest;
    recRow.nUserId = nUserId;
    recRow.nVersion = 1;
    m_mapRows[nUserId] = recRow;
    m_nRows.fetch_add(1);

    spOp->nUserId = nUserId;
    spOp->recResult = recRow;
    spOp->strTrace += "写表(插入 id=" + std::to_string(nUserId) + ");";
    return common::async::CPromiseResult::Resolve();
}

/// @brief 处理器（then）：写表（乐观锁更新）。
///
/// @param spOp 操作上下文（recRequest.nVersion 为期望版本）。
///
/// @return 兑现（版本 +1）；不存在返回 CDbError(kRowNotFound)；版本不匹配返回
///         CDbError(kVersionConflict)，并把库中最新行写入 recResult 供上层重试。
common::async::CPromiseResult CExampleDbModule::StepApplyUpdate(const std::shared_ptr<CUserTableOp>& spOp)
{
    SimulateDbIo(m_nLatencyMs);

    // 写链独占：读行 + 比对版本 + 写回都在同一个独占区里，不会有人插队 → 不需要锁。
    std::map<std::uint64_t, CUserRecord>::iterator it = m_mapRows.find(spOp->nUserId);
    if (it == m_mapRows.end())
    {
        spOp->strTrace += "更新失败(记录不存在);";
        return common::async::CPromiseResult::Reject(CDbError(CDbError::kRowNotFound, "记录不存在（更新）"));
    }
    if (it->second.nVersion != spOp->recRequest.nVersion)
    {
        spOp->recResult = it->second;  // 回吐库中最新行：上层据此重读并重试。
        spOp->strTrace +=
            "版本冲突(库=" + std::to_string(it->second.nVersion) + ",期望=" + std::to_string(spOp->recRequest.nVersion) + ");";
        return common::async::CPromiseResult::Reject(CDbError(CDbError::kVersionConflict, "乐观锁版本冲突（库中最新行已回吐）"));
    }

    CUserRecord recRow = spOp->recRequest;
    recRow.nVersion = it->second.nVersion + 1;
    it->second = recRow;

    spOp->recResult = recRow;
    spOp->strTrace += "写表(更新 version=" + std::to_string(recRow.nVersion) + ");";
    return common::async::CPromiseResult::Resolve();
}

/// @brief 处理器（then）：写表（删除行）。
///
/// @param spOp 操作上下文（nUserId 为目标用户）。
///
/// @return 兑现；不存在返回 CDbError(kRowNotFound)。
common::async::CPromiseResult CExampleDbModule::StepEraseRow(const std::shared_ptr<CUserTableOp>& spOp)
{
    SimulateDbIo(m_nLatencyMs);

    // 写链独占 → 不需要锁。
    std::map<std::uint64_t, CUserRecord>::iterator it = m_mapRows.find(spOp->nUserId);
    if (it == m_mapRows.end())
    {
        spOp->strTrace += "删除失败(记录不存在);";
        return common::async::CPromiseResult::Reject(CDbError(CDbError::kRowNotFound, "记录不存在（删除）"));
    }
    m_mapRows.erase(it);
    m_nRows.fetch_sub(1);
    spOp->strTrace += "写表(删除 id=" + std::to_string(spOp->nUserId) + ");";
    return common::async::CPromiseResult::Resolve();
}

/// @brief 处理器（finally）：模拟释放连接 / 记录慢查询。
///
/// @param upResult 上一层结果（可能是拒绝：finally 层失败也会执行）。
/// @param spOp 操作上下文（追加轨迹）。
///
/// @return upResult（finally 忽略返回值，结果原样透传）。
common::async::CPromiseResult CExampleDbModule::StepReleaseConn(
    common::async::CPromiseResult upResult, const std::shared_ptr<CUserTableOp>& spOp)
{
    // finally 层：成败都执行，upResult 可能是拒绝（返回值被忽略，原样透传）。
    spOp->strTrace += "放连接;";
    return upResult;
}

/// @brief 绑定成员函数为 then 处理器。
///
/// 处理器在数据访问模块的执行器线程上运行，模块生命周期由 Stop 保证（Stop 等待任务完成），
/// 因此处理器内使用 this 访问表数据是安全的。
///
/// @param pfnHandler 成员函数指针（then 签名）。
///
/// @return 可直接传给 NewPromise / Then 的处理器。
CExampleDbModule::ThenHandler CExampleDbModule::BindThen(ThenMemberFn pfnHandler)
{
    return [this, pfnHandler](const std::shared_ptr<CUserTableOp>& spOp)
    {
        return (this->*pfnHandler)(spOp);
    };
}

/// @brief 绑定成员函数为 catch / finally 处理器。
///
/// @param pfnHandler 成员函数指针（catch / finally 签名：多一个上游结果）。
///
/// @return 可直接传给 Catch / Finally 的处理器。
CExampleDbModule::ResultHandler CExampleDbModule::BindResult(ResultMemberFn pfnHandler)
{
    return [this, pfnHandler](common::async::CPromiseResult upResult, const std::shared_ptr<CUserTableOp>& spOp)
    {
        return (this->*pfnHandler)(upResult, spOp);
    };
}

}  // namespace serverexample
