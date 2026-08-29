#include "TestFramework.h"

#include <chrono>
#include <map>
#include <string>

namespace testfw {

/// @brief 全局用例注册表。
std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> s_registry;
    return s_registry;
}

/// @brief 注册测试用例。
///
/// @param strName 用例名称。
/// @param fnBody 用例执行体。
void Register(const char* strName, const std::function<void()>& fnBody)
{
    TestCase testCase;
    testCase.strName = strName;
    testCase.fnBody = fnBody;
    Registry().push_back(testCase);
}

/// @brief 运行全部用例（带计时与分组汇总表格）。
///
/// 输出：逐用例 [RUN]/[PASS]/[FAIL]（附耗时），结束后打印按前缀分组的汇总表。
///
/// @return 失败用例数（0 表示全部通过）。
int RunAll()
{
    std::vector<TestCase>& registry = Registry();
    int nPass = 0;
    int nFail = 0;

    // 分组统计（按名字第一个 '_' 前的前缀；无 '_' → "(other)"）。
    std::map<std::string, int> mapGroupTotal;
    std::map<std::string, int> mapGroupFail;
    std::map<std::string, double> mapGroupMs;

    std::printf("==========================================================\n");
    for (size_t i = 0; i < registry.size(); ++i)
    {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        std::printf("[ RUN  ] %s\n", registry[i].strName);
        bool bOk = true;
        try
        {
            registry[i].fnBody();
            ++nPass;
        }
        catch (...)
        {
            bOk = false;
            ++nFail;
        }
        std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
        double dMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        std::printf("[ %s ] %s  (%.1f ms)\n",
                    bOk ? "PASS" : "FAIL", registry[i].strName, dMs);

        // 分组统计。
        std::string strGroup = registry[i].strName;
        size_t nUnd = strGroup.find('_');
        if (nUnd != std::string::npos)
        {
            strGroup = strGroup.substr(0, nUnd);
        }
        mapGroupTotal[strGroup]++;
        if (!bOk)
        {
            mapGroupFail[strGroup]++;
        }
        mapGroupMs[strGroup] += dMs;
    }

    // ---- 汇总表格 ----
    std::printf("==========================================================\n");
    std::printf("Test Group Summary\n");
    std::printf("  %-16s %6s %6s %6s %12s\n",
                "Group", "Total", "Pass", "Fail", "Time(ms)");
    std::printf("  -------------------------------------------------------\n");
    int nTotalAll = 0;
    int nFailAll = 0;
    double dMsAll = 0.0;
    for (std::map<std::string, int>::const_iterator it = mapGroupTotal.begin();
         it != mapGroupTotal.end(); ++it)
    {
        const std::string& strGroup = it->first;
        int nGTotal = it->second;
        int nGFail = mapGroupFail[strGroup];
        double dGMs = mapGroupMs[strGroup];
        std::printf("  %-16s %6d %6d %6d %12.1f\n",
                    strGroup.c_str(), nGTotal, nGTotal - nGFail, nGFail, dGMs);
        nTotalAll += nGTotal;
        nFailAll += nGFail;
        dMsAll += dGMs;
    }
    std::printf("  -------------------------------------------------------\n");
    std::printf("  %-16s %6d %6d %6d %12.1f\n",
                "TOTAL", nTotalAll, nTotalAll - nFailAll, nFailAll, dMsAll);
    std::printf("==========================================================\n");
    std::printf("total=%d  pass=%d  fail=%d\n", nTotalAll, nPass, nFail);
    return nFail;
}

} // namespace testfw
