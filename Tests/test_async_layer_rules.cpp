/// @file test_async_layer_rules.cpp
/// then / catch / finally 的**三态语义**单测（白盒：直接盯住 `detail::ShouldPassThrough` /
/// `detail::ResolveLayerResult`）。
///
/// 为什么白盒：这两条规则以前散在 `CPromise::Append` 的 lambda 与 `MakeHandlerRunner` 里，
/// 只能靠集成用例间接覆盖；集中成纯函数后按组合表直测 —— 谁改动了 JS 三态语义，这里立刻红。

#include <memory>

#include "Async/Promise.h"
#include "Async/PromiseResult.h"
#include "TestFramework.h"

namespace no = common::async;

/// @brief 「本层是否跳过」（跳过 = 把上一层结果原样交给下一层）。
TEST(LayerRules_ShouldPassThrough)
{
    const no::CPromiseResult ok = no::CPromiseResult::Resolve();
    const no::CPromiseResult fail = no::CPromiseResult::Reject(no::kBusinessBase + 1);

    // then：上一层兑现才执行；被拒绝 → 跳过（失败即停）。
    ASSERT_TRUE(!no::detail::ShouldPassThrough(no::detail::kModeThen, ok));
    ASSERT_TRUE(no::detail::ShouldPassThrough(no::detail::kModeThen, fail));

    // catch：只在上一层被拒绝时执行；已兑现 → 跳过。
    ASSERT_TRUE(!no::detail::ShouldPassThrough(no::detail::kModeCatch, fail));
    ASSERT_TRUE(no::detail::ShouldPassThrough(no::detail::kModeCatch, ok));

    // finally：无论成败都执行。
    ASSERT_TRUE(!no::detail::ShouldPassThrough(no::detail::kModeFinally, ok));
    ASSERT_TRUE(!no::detail::ShouldPassThrough(no::detail::kModeFinally, fail));
}

/// @brief 「本层最终结果」（finally 忽略处理器返回值，原样透传上一层结果）。
TEST(LayerRules_ResolveLayerResult)
{
    const no::CPromiseResult ok = no::CPromiseResult::Resolve();
    const no::CPromiseResult fail = no::CPromiseResult::Reject(no::kBusinessBase + 2);

    // then：处理器返回什么就是什么（拒绝即停）。
    ASSERT_EQ(no::detail::ResolveLayerResult(no::detail::kModeThen, ok, fail).Code(), fail.Code());
    ASSERT_EQ(no::detail::ResolveLayerResult(no::detail::kModeThen, ok, ok).Code(), no::kFulfilled);

    // catch：可恢复 —— 处理器返回 Resolve() 即吞掉拒绝，链继续。
    ASSERT_EQ(no::detail::ResolveLayerResult(no::detail::kModeCatch, fail, ok).Code(), no::kFulfilled);
    ASSERT_EQ(no::detail::ResolveLayerResult(no::detail::kModeCatch, fail, fail).Code(), fail.Code());

    // finally：忽略处理器返回值，原样透传上一层结果。
    ASSERT_EQ(no::detail::ResolveLayerResult(no::detail::kModeFinally, fail, ok).Code(), fail.Code());
    ASSERT_EQ(no::detail::ResolveLayerResult(no::detail::kModeFinally, ok, fail).Code(), no::kFulfilled);
}
