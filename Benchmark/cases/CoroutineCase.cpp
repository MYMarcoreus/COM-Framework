#include "CoroutineCase.h"

#include "Async/AsyncExecutor.h"
#include "Async/Coroutine.h"
#include "framework/Bench.h"

#include <memory>
#include <string>

namespace {

/// 简单协程：启动后一次 CO_AWAIT 落地值，再 CO_RETURN。
class BenchCoroOnce : public common::async::CCoroutine<int>
{
public:
    int m_value = 0;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_value, []() { return 42; });
        CO_RETURN(m_value);
        CO_END();
    }
};

/// 10 级 await 链协程：每级 +1，验证多次挂起 / 恢复。
class BenchCoroChain10 : public common::async::CCoroutine<int>
{
public:
    int m_v = 0;

    void Run() override
    {
        CO_BEGIN();
        CO_AWAIT_INTO(m_v, []() { return 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_AWAIT_INTO(m_v, [this]() { return m_v + 1; });
        CO_RETURN(m_v);
        CO_END();
    }
};

} // namespace

void RunCoroutineCases()
{
    const std::string group = "3. 协程（CCoroutine 无栈协程）";
    common::async::CAsyncExecutor exec(1);
    exec.Start();

    // 基线：直接函数调用。
    benchmark::BenchOp(group, "direct_call (baseline)",
        []() { volatile int s = 42; (void)s; }, 7, "直接调用");

    // 等价单任务：Submit + Get。
    benchmark::BenchOp(group, "CAsyncExecutor single task",
        [&exec]() {
            volatile int s = exec.Submit([]() { return 42; }).Get().Value();
            (void)s;
        },
        7, "提交单个任务并取值");

    // 简单协程：CoStart + 一次 await + 完成。
    benchmark::BenchOp(group, "CCoroutine start+await+done",
        [&exec]() {
            std::shared_ptr<BenchCoroOnce> p = exec.CoStart<BenchCoroOnce>();
            volatile int s = p->Get().Value();
            (void)s;
        },
        7, "CoStart → 一次 CO_AWAIT → CO_RETURN");

    // 10 级链：直接函数。
    benchmark::BenchOp(group, "direct chain x10",
        []() {
            volatile int s;
            int v = 0;
            for (int i = 0; i < 10; ++i)
                v = v + 1;
            s = v;
            (void)s;
        },
        7, "循环 10 次");

    // 10 级链：CAsyncExecutor Then。
    benchmark::BenchOp(group, "CAsyncExecutor chain x10",
        [&exec]() {
            auto task = exec.Submit([]() { return 0; });
            for (int i = 0; i < 10; ++i)
                task = task.Then([](int x) { return x + 1; });
            volatile int s = task.Get().Value();
            (void)s;
        },
        5, "10 级 Then 链");

    // 10 级链：协程 10 次 await。
    benchmark::BenchOp(group, "CCoroutine chain x10 (10 await)",
        [&exec]() {
            std::shared_ptr<BenchCoroChain10> p = exec.CoStart<BenchCoroChain10>();
            volatile int s = p->Get().Value();
            (void)s;
        },
        5, "10 次 CO_AWAIT_INTO 挂起/恢复");

    exec.Stop();
}
