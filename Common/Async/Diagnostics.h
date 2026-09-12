#pragma once

#include <functional>

// ====================================================================
// 异步框架诊断钩子（进程级，独立于执行器 / promise / 协程）
//
// 用途：把「不致命、但肯定是用法 bug」的情况报出来 —— 通知里抛异常、在层内阻塞等待
// 未落定的层、在无效 promise 上挂层、Post 空任务等。应用可接日志 / 指标，测试可接断言。
//
// 为什么独立成文件：它不是执行器的能力 —— **promise / 协程 / 执行器都往这里报**，
// 钩子本身是进程级单例（框架内所有异步对象共用一个）。
//
// 默认策略（未设处理器时）：debug 构建打印到 stderr，发布构建安静。
// 报告自身不会抛异常（否则报告问题的手段会反过来弄坏框架）。
// ====================================================================

namespace common {
namespace async {

/// @brief 诊断处理器：接收一句「框架检测到的用法问题」描述（不带换行）。
///
/// 处理器里抛异常会被框架吞掉（只当作报告失败，不影响业务链）。
using DiagnosticHandler = std::function<void(const char* strWhat)>;

/// @brief 设置诊断处理器（线程安全）。
///
/// @param fnHandler 处理器；传 `nullptr` 恢复默认（debug 构建打印到 stderr，发布构建忽略）；
///                  想完全关闭传一个空 lambda。
void SetDiagnosticHandler(const DiagnosticHandler& fnHandler);

/// @brief 报告一次诊断（框架内部用；未设处理器时按默认策略处理，自身不抛异常）。
///
/// @param strWhat 问题描述（静态字符串，生命周期无要求）。
void ReportDiagnostic(const char* strWhat);

}  // namespace async
}  // namespace common
