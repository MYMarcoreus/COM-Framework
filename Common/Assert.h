#pragma once

#include <cstdio>
#include <cstdlib>

// ====================================================================
// 框架契约断言（ASSERT / ASSERT_MSG）—— 全框架通用，不只异步用
//
// 用途：**内部不变量**与**调用前提**。断言被触发说明框架/模块自己有 bug，或调用方
// 绕过了公开 API（例如未传必需参数、在对象未启动时使用它、把 0 当成拒绝码）。
//
// **不要**用它做业务校验：业务错误走返回值 / 错误码 / 异常 / 日志（见
// .github/skills/cpp-development/SKILL.md §6）—— 那才是调用方能处理的通道。
//
// 生效条件（`FRAMEWORK_DEBUG`）：未定义 NDEBUG **且** 未开启优化（-O0）——
// 即「调试构建」。发布构建下 ASSERT 展开为 `(void)sizeof(expr)`：**不求值、零开销、
// 不引入分支**。两个条件同时要求是有意的：既符合 C++ 的标准语义（NDEBUG 关闭断言），
// 又能在「发布构建忘记加 -DNDEBUG」时兜底（-O2 一定关闭断言，绝不会把 abort 带上线）。
//
// 失败行为：打印「表达式 / 位置 / 函数」后 abort() —— 比裸 assert 多带函数名与可选说明，
// 也便于接日志系统排查。
//
// 用法：
// @code
// #include "Assert.h"
//
// ASSERT(pCore != nullptr);                                    // 内部不变量
// ASSERT_MSG(spContext != nullptr, "上下文必须由调用方传入");     // 带说明
// @endcode
// ====================================================================

/// 是否为「调试构建」（1 = 断言生效；0 = 断言编译掉）。
///
/// 全框架唯一的调试判定入口：`SourceLoc.h`（ASYNC_LOC 注册点）与
/// `Async/Diagnostics.cpp`（诊断默认打印）都用它，避免各处各写一套。
#if !defined(NDEBUG) && !defined(__OPTIMIZE__)
    #define FRAMEWORK_DEBUG 1
#else
    #define FRAMEWORK_DEBUG 0
#endif

namespace common {

/// @brief 断言失败：打印「表达式 / 位置 / 函数」后 abort。
///
/// @param strExpr 断言表达式文本（ASSERT_MSG 时已拼上说明）。
/// @param strFile 源文件（__FILE__）。
/// @param nLine 行号（__LINE__）。
/// @param strFunction 函数名（__PRETTY_FUNCTION__）。
inline void FailAssert(const char* strExpr, const char* strFile, int nLine, const char* strFunction)
{
    std::fprintf(stderr, "[ASSERT 失败] %s\n  位置: %s:%d\n  函数: %s\n", strExpr, strFile, nLine, strFunction);
    std::fflush(stderr);
    std::abort();
}

}  // namespace common

#if FRAMEWORK_DEBUG
/// @brief 断言表达式为真（调试构建生效；发布构建不生成任何代码）。
    #define ASSERT(expr)                                                              \
        do                                                                            \
        {                                                                             \
            if (!(expr))                                                              \
            {                                                                         \
                ::common::FailAssert(#expr, __FILE__, __LINE__, __PRETTY_FUNCTION__); \
            }                                                                         \
        } while (0)

    /// @brief 带说明的断言（说明会与表达式一起打印）。
    #define ASSERT_MSG(expr, strMsg)                                                                \
        do                                                                                          \
        {                                                                                           \
            if (!(expr))                                                                            \
            {                                                                                       \
                ::common::FailAssert(#expr " —— " strMsg, __FILE__, __LINE__, __PRETTY_FUNCTION__); \
            }                                                                                       \
        } while (0)
#else
    #define ASSERT(expr) ((void)sizeof(expr))
    #define ASSERT_MSG(expr, strMsg) ((void)sizeof(expr))
#endif
