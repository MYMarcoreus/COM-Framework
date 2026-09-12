#pragma once

#include "Assert.h"  // FRAMEWORK_DEBUG：全框架唯一的调试判定（调试构建才留注册点）

// ====================================================================
// 异步链注册点源码位置（调试信息）
//
// 仅非优化调试构建（-O0，未定义 __OPTIMIZE__）下启用：注册「层」时把
// __PRETTY_FUNCTION__ / __FILE__ / __LINE__ 存进该层对应的链段；在调试器里
// 查看链段状态，即可知道「这一层是哪个函数、哪个文件、第几行注册的」。
//
// 发布构建（-O2，定义 __OPTIMIZE__）退化为空位置：不存储、零开销。
//
// 层函数是固定签名的（层与层之间只传成功 / 失败），同一处代码往往注册多个
// lambda，因此注册点位置是定位「哪一层失败」最直接的手段。
// ====================================================================
#if FRAMEWORK_DEBUG
    #define ASYNC_DEBUG_TRACE 1
#endif

namespace common {
namespace async {

/// @brief 源码位置（异步层的注册点：函数名 / 文件 / 行号）。
///
/// 仅在调试构建下被链段保存；发布构建下恒为空（零开销）。
/// 调试时在 watch 里查看 szFunction / szFile / nLine 定位层的注册点。
struct CSourceLoc
{
    const char* szFunction;  ///< __PRETTY_FUNCTION__（注册点函数名）。
    const char* szFile;      ///< __FILE__（注册点文件）。
    int nLine;               ///< __LINE__（注册点行号）。

    /// @brief 默认：空位置。
    CSourceLoc() : szFunction(NULL), szFile(NULL), nLine(0)
    {}

    /// @brief 完整位置。
    ///
    /// @param pszFunction 函数名（__PRETTY_FUNCTION__）。
    /// @param pszFile 文件名（__FILE__）。
    /// @param nLine 行号（__LINE__）。
    CSourceLoc(const char* pszFunction, const char* pszFile, int nLine)
        : szFunction(pszFunction), szFile(pszFile), nLine(nLine)
    {}
};

}  // namespace async
}  // namespace common

// 起链 / 挂层时作为最后一个参数传入：
//   exec.NewPromise(spCtx, StepLoad, ASYNC_LOC)
//   p.Then(StepSave, ASYNC_LOC)
// 说明：__PRETTY_FUNCTION__ / __FILE__ 是编译期静态串（零分配、程序生命周期安全），
//       __LINE__ 可在同一函数内区分多个注册点。
#if defined(ASYNC_DEBUG_TRACE)
    #define ASYNC_LOC common::async::CSourceLoc(__PRETTY_FUNCTION__, __FILE__, __LINE__)
#else
    #define ASYNC_LOC common::async::CSourceLoc()
#endif
