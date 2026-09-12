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
CExampleDbModule::CExampleDbModule(int nLatencyMs)
    : sc::CModule("example-db"), m_nNextId(kFirstAutoId), m_nLatencyMs(nLatencyMs)
{
    if (m_nLatencyMs < 0)
    {
        m_nLatencyMs = 0;
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
    {
        std::lock_guard<std::mutex> lock(m_mutex);
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
    }

    // ② 自建执行器：promise 是模板（上下文类型固定为 CUserTableOp），无法放进
    //    IAsyncExecutor 虚接口，因此本模块自持具体执行器；本模块的层不占用调用方线程。
    m_pExecutor.reset(new common::async::CAsyncExecutor(2));
    if (!m_pExecutor->Start())
    {
        m_pExecutor.reset();
        return false;
    }

    common::log::CLogger::Instance().Info(
        "[数据访问] 模拟数据库已启动（种子数据 2 行，单次 IO 延迟 " + std::to_string(m_nLatencyMs) + "ms）");
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
void CExampleDbModule::Shutdown()
{
    Stop();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mapRows.clear();
}

/// @brief 状态报告。
///
/// @return 模块名 + 当前表行数。
std::string CExampleDbModule::GetStatus() const
{
    size_t nRows = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        nRows = m_mapRows.size();
    }
    return "example-db 行数=" + std::to_string(nRows);
}

/// @brief 异步查询用户：读表 → 放连接（finally：失败也执行）。
///
/// @param spOp 操作上下文（调用方提供，命中行写入 recResult）。
///
/// @return promise 句柄（未命中时最终以 kDbRowNotFound 被拒绝）。
common::async::CPromise<CUserTableOp> CExampleDbModule::QueryUserAsync(const std::shared_ptr<CUserTableOp>& spOp)
{
    // 契约：执行器在 Start() 中创建（失败即模块不可用），操作上下文由调用方提供且非空。
    ASSERT_MSG(m_pExecutor != nullptr, "模块未启动：没有执行器可调度，不应调用本接口");
    ASSERT_MSG(spOp != nullptr, "接口契约：操作上下文必须非空");
    return LoadRowAsync(spOp).Finally(BindHandler(&CExampleDbModule::StepReleaseConn), ASYNC_LOC);
}

/// @brief 异步插入用户：**复用本模块内的读表异步函数**（查重）→ 写表 → 放连接。
///
/// 读表未命中（kDbRowNotFound）由 catch 归一化为兑现，因此查重层 / 写表层照常执行；
/// 其他拒绝（如 kException）继续透传，写表层不执行。
///
/// @param spOp 操作上下文（recRequest 为待插入行；nUserId 为 0 时自增分配）。
///
/// @return promise 句柄（主键冲突时最终以 kDbDuplicateKey 被拒绝）。
common::async::CPromise<CUserTableOp> CExampleDbModule::InsertUserAsync(const std::shared_ptr<CUserTableOp>& spOp)
{
    ASSERT_MSG(m_pExecutor != nullptr, "模块未启动：没有执行器可调度，不应调用本接口");
    ASSERT_MSG(spOp != nullptr, "接口契约：操作上下文必须非空");
    return LoadRowAsync(spOp)  // 本模块内的异步函数（读表）
        .Catch(BindHandler(&CExampleDbModule::StepAcceptNotFound), ASYNC_LOC)  // 「不存在」归一化为兑现
        .Then(BindHandler(&CExampleDbModule::StepRejectIfExists), ASYNC_LOC)   // 查重
        .Then(BindHandler(&CExampleDbModule::StepInsertRow), ASYNC_LOC)        // 写表
        .Finally(BindHandler(&CExampleDbModule::StepReleaseConn), ASYNC_LOC);  // 收尾：放连接
}

/// @brief 异步更新用户：**复用本模块内的读表异步函数** → 乐观锁写表 → 放连接。
///
/// @param spOp 操作上下文（recRequest 为期望版本 + 新字段）。
///
/// @return promise 句柄（版本冲突时最终以 kDbVersionConflict 被拒绝，recResult 为库中最新行）。
common::async::CPromise<CUserTableOp> CExampleDbModule::UpdateUserAsync(const std::shared_ptr<CUserTableOp>& spOp)
{
    ASSERT_MSG(m_pExecutor != nullptr, "模块未启动：没有执行器可调度，不应调用本接口");
    ASSERT_MSG(spOp != nullptr, "接口契约：操作上下文必须非空");
    return LoadRowAsync(spOp)
        .Catch(BindHandler(&CExampleDbModule::StepAcceptNotFound), ASYNC_LOC)
        .Then(BindHandler(&CExampleDbModule::StepApplyUpdate), ASYNC_LOC)
        .Finally(BindHandler(&CExampleDbModule::StepReleaseConn), ASYNC_LOC);
}

/// @brief 异步删除用户：**复用本模块内的读表异步函数**（确认存在）→ 删行 → 放连接。
///
/// @param spOp 操作上下文（nUserId 为目标用户）。
///
/// @return promise 句柄（未命中时最终以 kDbRowNotFound 被拒绝）。
common::async::CPromise<CUserTableOp> CExampleDbModule::DeleteUserAsync(const std::shared_ptr<CUserTableOp>& spOp)
{
    ASSERT_MSG(m_pExecutor != nullptr, "模块未启动：没有执行器可调度，不应调用本接口");
    ASSERT_MSG(spOp != nullptr, "接口契约：操作上下文必须非空");
    return LoadRowAsync(spOp)
        .Catch(BindHandler(&CExampleDbModule::StepAcceptNotFound), ASYNC_LOC)
        .Then(BindHandler(&CExampleDbModule::StepEraseRow), ASYNC_LOC)
        .Finally(BindHandler(&CExampleDbModule::StepReleaseConn), ASYNC_LOC);
}

/// @brief 本模块内的异步函数：读表（取连接 → 读行）。
///
/// 被查询 / 插入 / 更新 / 删除四个对外异步函数复用：同上下文类型，直接追加 handler 即可，
/// 不需要嵌套起 promise，也不会阻塞任何线程。
///
/// @param spOp 操作上下文（命中行写入 recResult，并置 bFound）。
///
/// @return promise 句柄（未命中时最终以 kDbRowNotFound 被拒绝）。
common::async::CPromise<CUserTableOp> CExampleDbModule::LoadRowAsync(const std::shared_ptr<CUserTableOp>& spOp)
{
    return m_pExecutor->NewPromise(spOp, BindHandler(&CExampleDbModule::StepAcquireConn), ASYNC_LOC)
        .Then(BindHandler(&CExampleDbModule::StepLoadRow), ASYNC_LOC);
}

/// @brief 处理器：取连接 + 模拟数据库 IO 延迟。
///
/// then 层：上游被拒绝时框架**不会调用本层**（失败即停），因此无需判断 upResult；
/// 只有 catch（StepAcceptNotFound）与 finally（StepReleaseConn）才需要看它。
///
/// @param spOp 操作上下文（追加轨迹）。
///
/// @return 兑现。
common::async::CPromiseResult CExampleDbModule::StepAcquireConn(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CUserTableOp>& spOp)
{
    spOp->strTrace += "取连接;";
    SimulateDbIo(m_nLatencyMs);
    return common::async::CPromiseResult::Resolve();
}

/// @brief 处理器：读表。
///
/// @param upResult 上一层结果。
/// @param spOp 操作上下文（命中写 recResult / bFound；演示开关触发驱动异常）。
///
/// @return 命中兑现；未命中返回 kDbRowNotFound；驱动异常抛出（框架转为 kException）。
common::async::CPromiseResult CExampleDbModule::StepLoadRow(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CUserTableOp>& spOp)
{
    SimulateDbIo(m_nLatencyMs);

    if (spOp->bSimulateDbError)
    {
        spOp->strTrace += "驱动异常;";
        throw std::runtime_error("模拟数据库驱动异常");  // 框架捕获 → 本层以 kException 被拒绝。
    }

    CUserRecord recRow;
    bool bFound = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<std::uint64_t, CUserRecord>::const_iterator it = m_mapRows.find(spOp->nUserId);
        if (it != m_mapRows.end())
        {
            recRow = it->second;
            bFound = true;
        }
    }

    spOp->bFound = bFound;
    if (!bFound)
    {
        spOp->strTrace += "读表未命中(id=" + std::to_string(spOp->nUserId) + ");";
        return common::async::CPromiseResult::Reject(kDbRowNotFound);
    }

    spOp->recResult = recRow;
    spOp->strTrace += "读表命中;";
    return common::async::CPromiseResult::Resolve();
}

/// @brief 处理器（catch）：把「记录不存在」归一化为兑现，供写入流程继续执行。
///
/// @param upResult 上一层结果（可能是 kDbRowNotFound 的拒绝）。
/// @param spOp 操作上下文（追加轨迹）。
///
/// @return 上一层为 kDbRowNotFound 时返回 Resolve()（吞掉该拒绝）；否则原样透传。
common::async::CPromiseResult CExampleDbModule::StepAcceptNotFound(
    common::async::CPromiseResult upResult, const std::shared_ptr<CUserTableOp>& spOp)
{
    // catch 层：只在被拒绝时执行，upResult 必定是拒绝 —— 只有这里才必须看它。
    if (upResult.Code() == kDbRowNotFound)
    {
        spOp->strTrace += "归一化(未命中→兑现);";
        return common::async::CPromiseResult::Resolve();
    }
    return upResult;  // 其他拒绝（如 kException）继续透传：后续 Then 层不执行。
}

/// @brief 处理器：查重（已存在则本层拒绝）。
///
/// @param upResult 上一层结果。
/// @param spOp 操作上下文（读 bFound）。
///
/// @return 不存在兑现；存在返回 kDbDuplicateKey。
common::async::CPromiseResult CExampleDbModule::StepRejectIfExists(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CUserTableOp>& spOp)
{
    if (spOp->bFound)
    {
        spOp->strTrace += "主键冲突;";
        return common::async::CPromiseResult::Reject(kDbDuplicateKey);
    }
    return common::async::CPromiseResult::Resolve();
}

/// @brief 处理器：写表（插入行，必要时分配自增主键）。
///
/// @param upResult 上一层结果。
/// @param spOp 操作上下文（写入 recResult / nUserId）。
///
/// @return 兑现；主键冲突返回 kDbDuplicateKey。
common::async::CPromiseResult CExampleDbModule::StepInsertRow(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CUserTableOp>& spOp)
{
    SimulateDbIo(m_nLatencyMs);

    std::lock_guard<std::mutex> lock(m_mutex);
    std::uint64_t nUserId = spOp->recRequest.nUserId;
    if (nUserId == 0)
    {
        nUserId = m_nNextId++;  // 自增分配主键（模拟数据库自增列）。
    }
    if (m_mapRows.find(nUserId) != m_mapRows.end())
    {
        spOp->strTrace += "主键冲突;";
        return common::async::CPromiseResult::Reject(kDbDuplicateKey);  // 双保险：并发插入时兜底。
    }

    CUserRecord recRow = spOp->recRequest;
    recRow.nUserId = nUserId;
    recRow.nVersion = 1;
    m_mapRows[nUserId] = recRow;

    spOp->nUserId = nUserId;
    spOp->recResult = recRow;
    spOp->strTrace += "写表(插入 id=" + std::to_string(nUserId) + ");";
    return common::async::CPromiseResult::Resolve();
}

/// @brief 处理器：写表（乐观锁更新）。
///
/// @param upResult 上一层结果。
/// @param spOp 操作上下文（recRequest.nVersion 为期望版本）。
///
/// @return 兑现（版本 +1）；不存在返回 kDbRowNotFound；版本不匹配返回
///         kDbVersionConflict，并把库中最新行写入 recResult 供上层重试。
common::async::CPromiseResult CExampleDbModule::StepApplyUpdate(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CUserTableOp>& spOp)
{
    SimulateDbIo(m_nLatencyMs);

    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<std::uint64_t, CUserRecord>::iterator it = m_mapRows.find(spOp->nUserId);
    if (it == m_mapRows.end())
    {
        spOp->strTrace += "更新失败(记录不存在);";
        return common::async::CPromiseResult::Reject(kDbRowNotFound);
    }
    if (it->second.nVersion != spOp->recRequest.nVersion)
    {
        spOp->recResult = it->second;  // 回吐库中最新行：上层据此重读并重试。
        spOp->strTrace += "版本冲突(库=" + std::to_string(it->second.nVersion) +
                          ",期望=" + std::to_string(spOp->recRequest.nVersion) + ");";
        return common::async::CPromiseResult::Reject(kDbVersionConflict);
    }

    CUserRecord recRow = spOp->recRequest;
    recRow.nVersion = it->second.nVersion + 1;
    it->second = recRow;

    spOp->recResult = recRow;
    spOp->strTrace += "写表(更新 version=" + std::to_string(recRow.nVersion) + ");";
    return common::async::CPromiseResult::Resolve();
}

/// @brief 处理器：写表（删除行）。
///
/// @param upResult 上一层结果。
/// @param spOp 操作上下文（nUserId 为目标用户）。
///
/// @return 兑现；不存在返回 kDbRowNotFound。
common::async::CPromiseResult CExampleDbModule::StepEraseRow(
    common::async::CPromiseResult /*upResult*/, const std::shared_ptr<CUserTableOp>& spOp)
{
    SimulateDbIo(m_nLatencyMs);

    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<std::uint64_t, CUserRecord>::iterator it = m_mapRows.find(spOp->nUserId);
    if (it == m_mapRows.end())
    {
        spOp->strTrace += "删除失败(记录不存在);";
        return common::async::CPromiseResult::Reject(kDbRowNotFound);
    }
    m_mapRows.erase(it);
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

/// @brief 绑定成员函数为处理器。
///
/// 处理器在数据访问模块的执行器线程上运行，模块生命周期由 Stop 保证（Stop 等待任务完成），
/// 因此处理器内使用 this 访问表数据是安全的。
///
/// @param pfnHandler 成员函数指针（签名与处理器一致）。
///
/// @return 可直接传给 NewPromise / Then / Catch / Finally 的处理器。
CExampleDbModule::Handler CExampleDbModule::BindHandler(HandlerMemberFn pfnHandler)
{
    return [this, pfnHandler](common::async::CPromiseResult upResult, const std::shared_ptr<CUserTableOp>& spOp)
    {
        return (this->*pfnHandler)(upResult, spOp);
    };
}

}  // namespace serverexample
