/// @file Benchmark.h
/// 轻量基准测试框架（参考 Google Benchmark / Criterion 思路）。
///
/// 特点：
///  - BENCHMARK(name, ops) 静态注册基准（main 前完成）
///  - 测量每操作耗时（ns/op）：warmup 1 次 + 多次取最优（最小），降噪
///  - 结果输出为对比表格（name / ops / ms / ns/op / baseline / ratio / status）
///  - 与基准文件（benchmarks.txt）比较：ratio 超 ±kTolerance 判 FAIL（性能退化）
///  - ./tests --update-benchmark 重新校准基准文件
///
/// 度量：ns/op（每操作纳秒）。比较：current / baseline 比率，
/// ratio > 1 + kTolerance 视为性能退化（FAIL），提升（加速）不判失败。

#pragma once

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace bench {

/// @brief 基准条目（静态注册）。
struct CBenchEntry
{
    const char* szName;               ///< 基准名（表格第一列）。
    long nOps;                        ///< 操作数（用于计算 ns/op）。
    std::function<void()> fnBody;     ///< 被测操作体。
};

/// @brief 基准结果（测量后填充）。
struct CBenchResult
{
    const char* szName;
    long nOps;
    double dNsPerOp;                  ///< 每操作纳秒（<0 表示执行异常）。
};

/// @brief 全局基准注册表。
inline std::vector<CBenchEntry>& Registry()
{
    static std::vector<CBenchEntry> s_registry;
    return s_registry;
}

/// @brief 注册基准（BENCHMARK 宏调用）。
inline void Register(const char* szName, long nOps,
                     const std::function<void()>& fnBody)
{
    CBenchEntry entry;
    entry.szName = szName;
    entry.nOps = nOps;
    entry.fnBody = fnBody;
    Registry().push_back(entry);
}

/// @brief 与基准比较的允许偏差（0.40 = ±40%，超出视为性能退化）。
/// 并行场景（AwaitAll/Nested）在共享环境下波动可达 ±30%，取 0.40
/// 可抓明显退化（≥1.4x）同时容忍正常调度噪声；如需更严格可调低。
const double kTolerance = 0.40;

/// @brief 基准文件路径（相对当前工作目录）。
const char* const kBaselineFile = "benchmarks.txt";

/// @brief 测量单条目：warmup 1 次 + 测量 kRounds 次取最优（最小 ns/op）。
///
/// @param entry 基准条目。
/// @param bOk 输出：测量是否成功（fnBody 抛异常为 false）。
/// @return ns/op（执行异常返回 -1.0）。
inline double MeasureNsPerOp(CBenchEntry& entry, bool& bOk)
{
    bOk = true;
    try
    {
        entry.fnBody(); // warmup：预热缓存/分配。
    }
    catch (...)
    {
        bOk = false;
        return -1.0;
    }

    const int kRounds = 5; // 多次测量取最优，降低调度噪声。
    double dBest = 0.0;
    for (int r = 0; r < kRounds; ++r)
    {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        try
        {
            entry.fnBody();
        }
        catch (...)
        {
            bOk = false;
            return -1.0;
        }
        std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
        double dNs = std::chrono::duration<double, std::nano>(t1 - t0).count();
        double dPerOp = (entry.nOps > 0) ? (dNs / static_cast<double>(entry.nOps)) : 0.0;
        if (r == 0 || dPerOp < dBest)
        {
            dBest = dPerOp;
        }
    }
    return dBest;
}

/// @brief 读取基准文件（name -> ns/op）。
///
/// @param out 输出：基准名到 ns/op 的映射。
/// @return true 文件存在并成功读取。
inline bool LoadBaseline(std::vector<std::pair<std::string, double> >& out)
{
    std::ifstream f(kBaselineFile);
    if (!f.is_open())
    {
        return false;
    }
    std::string strLine;
    while (std::getline(f, strLine))
    {
        if (strLine.empty() || strLine[0] == '#')
        {
            continue;
        }
        size_t nEq = strLine.find('=');
        if (nEq == std::string::npos)
        {
            continue;
        }
        std::string strName = strLine.substr(0, nEq);
        size_t nBegin = strName.find_first_not_of(" \t");
        size_t nEnd = strName.find_last_not_of(" \t");
        if (nBegin == std::string::npos)
        {
            continue;
        }
        strName = strName.substr(nBegin, nEnd - nBegin + 1);
        double dValue = 0.0;
        std::sscanf(strLine.c_str() + nEq + 1, "%lf", &dValue);
        out.push_back(std::make_pair(strName, dValue));
    }
    return true;
}

/// @brief 保存基准文件（--update-benchmark 用）。
inline void SaveBaseline(const std::vector<CBenchResult>& results)
{
    std::ofstream f(kBaselineFile);
    if (!f.is_open())
    {
        std::printf("[BENCH] 无法写入基准文件 %s\n", kBaselineFile);
        return;
    }
    f << "# benchmarks.txt - 由 ./build/release/tests --update-benchmark 生成\n";
    f << "# 度量：ns/op（每操作纳秒）。与基准比较容差 ±"
      << static_cast<int>(kTolerance * 100) << "%\n";
    for (size_t i = 0; i < results.size(); ++i)
    {
        f << results[i].szName << " = " << results[i].dNsPerOp << "\n";
    }
    std::printf("[BENCH] 基准文件已保存：%s\n", kBaselineFile);
}

/// @brief 运行全部基准并输出对比表格。
///
/// @param bUpdate true 时更新基准文件（不判失败）。
/// @return 失败数（性能退化超容差 + 执行异常的条目数）。
inline int RunAll(bool bUpdate)
{
    std::vector<CBenchEntry>& registry = Registry();
    if (registry.empty())
    {
        std::printf("[BENCH] 无基准条目\n");
        return 0;
    }

    // ---- 测量全部 ----
    std::vector<CBenchResult> results;
    results.reserve(registry.size());
    std::printf("===============================================================\n");
    for (size_t i = 0; i < registry.size(); ++i)
    {
        std::printf("[BENCH] %-28s 测量中...\n", registry[i].szName);
        bool bOk = true;
        double dNsPerOp = MeasureNsPerOp(registry[i], bOk);
        CBenchResult res;
        res.szName = registry[i].szName;
        res.nOps = registry[i].nOps;
        res.dNsPerOp = dNsPerOp;
        results.push_back(res);
    }

    // ---- 加载基准 ----
    std::vector<std::pair<std::string, double> > baseline;
    bool bHasBaseline = LoadBaseline(baseline);

    // ---- 输出表格 ----
    std::printf("===============================================================\n");
    std::printf("%-24s %7s %8s %10s %10s %6s %s\n",
                "Benchmark", "Ops", "Time(ms)", "ns/op", "Baseline", "Ratio", "Status");
    std::printf("----------------------------------------------------------------\n");

    int nFail = 0;
    for (size_t i = 0; i < results.size(); ++i)
    {
        if (results[i].dNsPerOp < 0.0)
        {
            std::printf("%-24s %7ld %8s %10s %10s %6s %s\n",
                        results[i].szName, results[i].nOps,
                        "-", "-", "-", "-", "ERROR");
            ++nFail;
            continue;
        }

        double dTimeMs = results[i].dNsPerOp * static_cast<double>(results[i].nOps) / 1e6;

        // 查基准。
        double dBase = 0.0;
        bool bFound = false;
        for (size_t j = 0; j < baseline.size(); ++j)
        {
            if (baseline[j].first == results[i].szName)
            {
                dBase = baseline[j].second;
                bFound = true;
                break;
            }
        }

        const char* szStatus = "(new)";
        double dRatio = 0.0;
        if (bHasBaseline && bFound)
        {
            dRatio = results[i].dNsPerOp / dBase;
            if (dRatio > 1.0 + kTolerance)
            {
                szStatus = "FAIL"; // 性能退化超容差。
                ++nFail;
            }
            else
            {
                szStatus = "OK";
            }
        }

        if (bFound)
        {
            std::printf("%-24s %7ld %8.1f %10.1f %10.1f %6.2f %s\n",
                        results[i].szName, results[i].nOps, dTimeMs,
                        results[i].dNsPerOp, dBase, dRatio, szStatus);
        }
        else
        {
            std::printf("%-24s %7ld %8.1f %10.1f %10s %6s %s\n",
                        results[i].szName, results[i].nOps, dTimeMs,
                        results[i].dNsPerOp, "-", "-", szStatus);
        }
    }
    std::printf("----------------------------------------------------------------\n");

    if (bUpdate)
    {
        SaveBaseline(results);
        std::printf("[BENCH] total=%zu  更新完成（不判失败）\n", results.size());
        return 0;
    }

    if (!bHasBaseline)
    {
        std::printf("[BENCH] 无基准文件 %s；请运行 ./tests --update-benchmark 生成基准。\n",
                    kBaselineFile);
    }
    std::printf("[BENCH] total=%zu  fail=%d  容差=±%.0f%%\n",
                results.size(), nFail, kTolerance * 100.0);
    return nFail;
}

} // namespace bench

/// @brief 注册基准（静态注册，main 前自动完成）。
/// 用法：BENCHMARK(name, ops) { ...被测操作体... }
#define BENCHMARK(name, ops)                                              \
    static void Bench_##name();                                           \
    namespace {                                                           \
    struct BenchReg_##name                                                 \
    {                                                                     \
        BenchReg_##name() { bench::Register(#name, (ops), &Bench_##name); } \
    } g_BenchReg_##name;                                                   \
    }                                                                     \
    static void Bench_##name()
