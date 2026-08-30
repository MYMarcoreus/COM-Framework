// ====================================================================
// Markdown 表格汇总输出
//   - BuildMarkdown()：把 Registry 中的结果按分组渲染成 markdown。
//   - ReportToFiles()：打印到终端 + 写入
//     Benchmark/results/benchmark-report.md（路径由
//     -DBENCHMARK_RESULT_DIR 编译期指定）。
//
// 每个分组内以「name 含 baseline/direct 的行」作为基线，计算相对倍数
// （× 越小越好；baseline 自身显示 -）。
// ====================================================================
#ifndef COM_BENCHMARK_FRAMEWORK_REPORT_H
#define COM_BENCHMARK_FRAMEWORK_REPORT_H

#include "framework/Bench.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace benchmark {

namespace {

inline std::string NowStamp()
{
    std::time_t t = std::time(nullptr);
    std::tm tm = *std::localtime(&t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

/// 读取 CPU 型号（/proc/cpuinfo 第一处 model name）。
inline std::string CpuModel()
{
    std::ifstream ifs("/proc/cpuinfo");
    std::string line;
    while (std::getline(ifs, line))
    {
        if (line.compare(0, 10, "model name") == 0)
        {
            size_t pos = line.find(':');
            if (pos != std::string::npos)
                return line.substr(pos + 2);
        }
    }
    return "unknown";
}

/// 在线 CPU 核数。
inline int CoreCount()
{
    long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? static_cast<int>(n) : 1;
}

inline std::string FmtDouble(double v, int prec)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

/// 吞吐量格式化（K / M / G 单位）。
inline std::string FmtOps(double ops)
{
    if (ops >= 1e9)
        return FmtDouble(ops / 1e9, 2) + " G";
    if (ops >= 1e6)
        return FmtDouble(ops / 1e6, 2) + " M";
    if (ops >= 1e3)
        return FmtDouble(ops / 1e3, 2) + " K";
    return FmtDouble(ops, 2);
}

/// 取分组内的基线均值（name 含 baseline / direct 的首行）。
inline double GroupBaseline(const std::vector<Result>& results, const std::string& group)
{
    for (size_t i = 0; i < results.size(); ++i)
    {
        const Result& r = results[i];
        if (r.group == group &&
            (r.name.find("baseline") != std::string::npos ||
             r.name.find("direct") != std::string::npos))
        {
            if (r.mean_ns > 0.0)
                return r.mean_ns;
        }
    }
    return 0.0;
}

} // namespace

/// 渲染全部结果为 markdown 字符串。
inline std::string BuildMarkdown()
{
    const std::vector<Result>& results = Registry::Instance().Results();
    std::ostringstream os;

    os << "# 异步 / 协程库性能测试报告\n\n";
    os << "- 生成时间：" << NowStamp() << "\n";
    os << "- 环境：" << CpuModel() << "（" << CoreCount() << " 核在线）\n";
    os << "- 构建：`./build.sh -r Benchmark`（release / -O2）；debug 请用 `-d`\n";
    os << "- 被测：Common::thread::CThreadPool / Common::async::CAsyncExecutor /\n"
       << "  Common::async::CCoroutine；对比 asio::post（本项目自带第三方库）\n";
    os << "- 说明：微基准报告 ns/op；压力测试报告稳定窗口吞吐（ops/s）\n\n";

    std::string curGroup;
    for (size_t i = 0; i < results.size(); ++i)
    {
        const Result& r = results[i];
        if (r.group != curGroup)
        {
            curGroup = r.group;
            os << "\n## " << r.group << "\n\n";
            if (r.is_stress)
            {
                if (r.ops_per_sec > 0.0)
                {
                    os << "| 实现 | 稳定吞吐(ops/s) | 平均单任务耗时(μs) | 说明 |\n";
                    os << "|---|---|---|---|\n";
                }
                else
                {
                    os << "| 实现 | 阻塞耗时(ms) | 说明 |\n";
                    os << "|---|---|---|\n";
                }
            }
            else
            {
                os << "| 实现 | 均值(ns/op) | P50(ns) | P90(ns) | P99(ns) | stddev(ns) | 吞吐(ops/s) | 相对基线 | 说明 |\n";
                os << "|---|---|---|---|---|---|---|---|---|\n";
            }
        }

        if (r.is_stress)
        {
            if (r.ops_per_sec > 0.0)
            {
                os << "| " << r.name
                   << " | " << FmtOps(r.ops_per_sec)
                   << " | " << FmtDouble(r.mean_ns / 1e3, 2)
                   << " | " << r.note << " |\n";
            }
            else
            {
                os << "| " << r.name
                   << " | " << FmtDouble(r.mean_ns / 1e6, 3)
                   << " | " << r.note << " |\n";
            }
        }
        else
        {
            double base = GroupBaseline(results, r.group);
            std::string rel = "-";
            if (base > 0.0)
                rel = FmtDouble(r.mean_ns / base, 1) + "×";
            os << "| " << r.name
               << " | " << FmtDouble(r.mean_ns, 1)
               << " | " << FmtDouble(r.p50_ns, 1)
               << " | " << FmtDouble(r.p90_ns, 1)
               << " | " << FmtDouble(r.p99_ns, 1)
               << " | " << FmtDouble(r.stddev_ns, 1)
               << " | " << FmtOps(r.ops_per_sec)
               << " | " << rel
               << " | " << r.note << " |\n";
        }
    }
    return os.str();
}

/// 打印表格并写入结果目录（返回 0 成功）。
inline int ReportToFiles()
{
    std::string md = BuildMarkdown();
    std::cout << md;

#ifndef BENCHMARK_RESULT_DIR
#define BENCHMARK_RESULT_DIR "."
#endif
    const std::string dir = BENCHMARK_RESULT_DIR;
    ::mkdir(dir.c_str(), 0755); // 目录不存在则创建。
    const std::string path = dir + "/benchmark-report.md";
    std::ofstream ofs(path.c_str());
    if (ofs)
    {
        ofs << md;
        std::cout << "\n[结果已写入] " << path << "\n";
        return 0;
    }
    std::cerr << "[错误] 无法写入 " << path << "\n";
    return 1;
}

} // namespace benchmark

#endif // COM_BENCHMARK_FRAMEWORK_REPORT_H
