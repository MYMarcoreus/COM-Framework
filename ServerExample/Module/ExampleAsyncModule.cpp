#include "Module/ExampleAsyncModule.h"

#include <memory>
#include <stdexcept>
#include <string>

#include "Async/AsyncChain.h"
#include "Async/Coroutine.h"
#include "Infra/GuardedTimer.h"
#include "Log/Logger.h"
#include "Module/ResolveContext.h"

namespace serverexample {

namespace {

namespace no = common::async;

/// 演示用业务错误码（业务码从 kStepBusinessBase 起取）。
enum DemoStepCode
{
    kDemoInvalidParam = no::kStepBusinessBase + 1,  ///< 输入参数非法。
    kDemoStoreFailed = no::kStepBusinessBase + 2    ///< 落库失败。
};

/// @brief 演示流程的共享上下文（链内所有层、协程与子链共用同一实例）。
///
/// 这是「层间不传任意值」后的数据载体：各层的输入 / 输出全部写在这里，
/// 层与层之间只传递本层的成功 / 失败。
struct CAsyncDemoContext
{
    int nBase;             ///< 输入基数（读参数层写入）。
    int nScaled;           ///< 缩放结果（缩放层写入）。
    bool bStoreFailed;     ///< 是否模拟落库失败。
    bool bRolledBack;      ///< 是否已回滚（ThenAlways 层写入）。
    std::string strTrace;  ///< 层执行轨迹。

    CAsyncDemoContext() : nBase(10), nScaled(0), bStoreFailed(false), bRolledBack(false) {}
};

// ---------------- 层函数（固定签名：上一层结果 + 共享上下文 → 本层结果） ----------------

/// @brief 层：读参数（写入上下文；参数非法则本层失败）。
no::CStepResult StepReadParam(no::CStepResult upStep, const std::shared_ptr<CAsyncDemoContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;  // 上一层已失败：透传（链在失败即停时不会调用本层，属防御写法）。
    }
    spCtx->strTrace += "读参数;";
    if (spCtx->nBase <= 0)
    {
        return no::CStepResult::Failed(kDemoInvalidParam);
    }
    return no::CStepResult::Ok();
}

/// @brief 层：缩放计算（基数 ×3 写入上下文）。
no::CStepResult StepScale(no::CStepResult upStep, const std::shared_ptr<CAsyncDemoContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    spCtx->nScaled = spCtx->nBase * 3;
    spCtx->strTrace += "缩放;";
    return no::CStepResult::Ok();
}

/// @brief 层：落库（可模拟失败）。
no::CStepResult StepStore(no::CStepResult upStep, const std::shared_ptr<CAsyncDemoContext>& spCtx)
{
    if (upStep.IsFailed())
    {
        return upStep;
    }
    spCtx->strTrace += "落库;";
    if (spCtx->bStoreFailed)
    {
        return no::CStepResult::Failed(kDemoStoreFailed);
    }
    return no::CStepResult::Ok();
}

/// @brief 层（ThenAlways）：回滚 / 清理——无论成败都会执行，并看得到上一层的结果。
no::CStepResult StepRollback(no::CStepResult upStep, const std::shared_ptr<CAsyncDemoContext>& spCtx)
{
    spCtx->strTrace += "回滚;";
    if (upStep.IsFailed())
    {
        spCtx->bRolledBack = true;
        return upStep;  // 透传失败：后续 Then 层仍不执行。
    }
    return no::CStepResult::Ok();
}

/// @brief 层：抛异常（演示框架捕获后转为本层失败 kStepException）。
no::CStepResult StepThrow(no::CStepResult /*upStep*/, const std::shared_ptr<CAsyncDemoContext>& /*spCtx*/)
{
    throw std::runtime_error("演示用异常");
}

/// @brief 层：被调用即留下痕迹（验证失败即停时未执行）。
no::CStepResult StepCleanupAfterStore(no::CStepResult upStep, const std::shared_ptr<CAsyncDemoContext>& spCtx)
{
    (void)upStep;
    spCtx->strTrace += "清理;";
    return no::CStepResult::Ok();
}

/// @brief 演示协程：用顺序代码 await 两条子链（复用模块执行器与共享上下文）。
class CDemoCoroutine : public no::CCoroutine<CAsyncDemoContext>
{
   public:
    using no::CCoroutine<CAsyncDemoContext>::CCoroutine;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT(Chain(&StepReadParam));  // 等待子链（失败则本协程终止）
        CO_AWAIT(Chain(&StepScale));
        CO_AWAIT(Chain(&StepStore));
        CO_RETURN_VOID();
        CO_END();
    }
};

}  // namespace

/// @brief 创建异步演示模块。
///
/// @param intervalMs 演示周期（毫秒，小于 100 按 100 处理）。
CExampleAsyncModule::CExampleAsyncModule(std::int64_t intervalMs)
    : sc::CModule("async-example"), m_nIntervalMs(intervalMs), m_tTimerId(common::timer::kInvalidTimerId)
{
    // 依赖定时器接口模块：周期触发演示。
    AddDependency(sc::IID_ITimer());
    if (m_nIntervalMs < 100)
    {
        m_nIntervalMs = 100;
    }
}

/// @brief 销毁异步演示模块。
CExampleAsyncModule::~CExampleAsyncModule()
{
    Stop();
}

/// @brief 解析 ITimer 接口。
///
/// @param ctx 初始化上下文（依赖注入）。
///
/// @return true 定时器接口就绪；false 缺失。
bool CExampleAsyncModule::Initialize(const sc::CResolveContext& ctx)
{
    m_pTimer.Reset(ctx.Resolve<sc::ITimer>());
    return m_pTimer != nullptr;
}

/// @brief 启动自建执行器并注册周期演示定时器。
///
/// 自建 common::async::CAsyncExecutor：链与协程都是模板（上下文类型各异），
/// 无法放进 IAsyncExecutor 接口的虚函数表，因此演示模块自持一个具体执行器。
///
/// @return true 启动成功；false 执行器或定时器接口缺失。
bool CExampleAsyncModule::Start()
{
    if (m_pTimer == nullptr)
    {
        return false;
    }
    m_pExecutor.reset(new common::async::CAsyncExecutor(2));
    if (!m_pExecutor->Start())
    {
        return false;
    }
    // 周期演示：弱引用守卫，模块停止/销毁后回调自动跳过。
    m_tTimerId =
        sc::AddGuardedPeriodicTimer(m_pTimer.Get(), m_nIntervalMs, WeakSelf<CExampleAsyncModule>(),
                                    [](const sc::ScopedInterfacePtr<CExampleAsyncModule>& sp) { sp->RunExample(); });
    return true;
}

/// @brief 取消定时器并停止执行器。
///
/// CAsyncExecutor::Stop 等待已投递任务完成（优雅关闭）。
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
    if (m_pExecutor != nullptr)
    {
        m_pExecutor->Stop();
        m_pExecutor.reset();
    }
}

/// @brief 停止并释放接口引用。
void CExampleAsyncModule::Shutdown()
{
    Stop();
    m_pTimer.Reset();
}

/// @brief 执行一次完整的异步能力演示。
///
/// 覆盖：链 + 共享上下文、失败即停、ThenAlways 失败可见、异常转失败、协程顺序 await。
void CExampleAsyncModule::RunExample()
{
    if (m_pExecutor == nullptr)
    {
        return;
    }
    common::log::CLogger& logger = common::log::CLogger::Instance();

    // ① 链 + 共享上下文：数据写上下文，层间只传成功 / 失败。
    std::shared_ptr<CAsyncDemoContext> spCtx = std::make_shared<CAsyncDemoContext>();
    spCtx->nBase = 10;
    const no::CStepResult stepOk = m_pExecutor->Submit(spCtx, &StepReadParam, ASYNC_LOC)
                                       .Then(&StepScale, ASYNC_LOC)
                                       .Then(&StepStore, ASYNC_LOC)
                                       .Get();
    logger.Info("[AsyncExample] 链成功=" + std::string(stepOk.IsOk() ? "是" : "否") +
                " 缩放结果=" + std::to_string(spCtx->nScaled) + " 轨迹=" + spCtx->strTrace);

    // ② 失败即停：参数非法 → 后续层不执行，业务失败码透传到 Get()。
    std::shared_ptr<CAsyncDemoContext> spFailCtx = std::make_shared<CAsyncDemoContext>();
    spFailCtx->nBase = -1;
    const no::CStepResult stepFail = m_pExecutor->Submit(spFailCtx, &StepReadParam, ASYNC_LOC)
                                         .Then(&StepScale, ASYNC_LOC)  // 不执行
                                         .Then(&StepStore, ASYNC_LOC)  // 不执行
                                         .Get();
    logger.Warn("[AsyncExample] 失败即停 码=" + std::to_string(stepFail.Code()) + " 轨迹=" + spFailCtx->strTrace);

    // ③ ThenAlways：失败也执行（回滚 / 清理），upStep 携带上一层失败状态。
    std::shared_ptr<CAsyncDemoContext> spRollbackCtx = std::make_shared<CAsyncDemoContext>();
    spRollbackCtx->bStoreFailed = true;
    const no::CStepResult stepRollback = m_pExecutor->Submit(spRollbackCtx, &StepReadParam, ASYNC_LOC)
                                             .Then(&StepScale, ASYNC_LOC)
                                             .Then(&StepStore, ASYNC_LOC)
                                             .ThenAlways(&StepRollback, ASYNC_LOC)     // 失败也执行
                                             .Then(&StepCleanupAfterStore, ASYNC_LOC)  // 失败仍在 → 不执行
                                             .Get();
    logger.Warn("[AsyncExample] ThenAlways 回滚=" + std::string(spRollbackCtx->bRolledBack ? "已执行" : "未执行") +
                " 码=" + std::to_string(stepRollback.Code()) + " 轨迹=" + spRollbackCtx->strTrace);

    // ④ 层内异常 → 本层失败（kStepException），不向调用方抛出。
    std::shared_ptr<CAsyncDemoContext> spThrowCtx = std::make_shared<CAsyncDemoContext>();
    const no::CStepResult stepThrow = m_pExecutor->Submit(spThrowCtx, &StepThrow, ASYNC_LOC).Get();
    logger.Warn("[AsyncExample] 异常转失败 码=" + std::to_string(stepThrow.Code()));

    // ⑤ 协程：顺序 await 三条子链（子链与协程共享同一上下文）。
    std::shared_ptr<CAsyncDemoContext> spCoroCtx = std::make_shared<CAsyncDemoContext>();
    spCoroCtx->nBase = 7;
    std::shared_ptr<CDemoCoroutine> pCoro = m_pExecutor->CoStart<CDemoCoroutine>(spCoroCtx);
    const no::CStepResult stepCoro = pCoro->Get();
    logger.Info("[AsyncExample] 协程=" + std::string(stepCoro.IsOk() ? "成功" : "失败") +
                " 缩放结果=" + std::to_string(spCoroCtx->nScaled) + " 轨迹=" + spCoroCtx->strTrace);
}

}  // namespace serverexample
