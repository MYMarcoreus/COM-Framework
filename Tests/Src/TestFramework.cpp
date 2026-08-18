#include "TestFramework.h"

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

/// @brief 运行全部用例。
///
/// @return 失败用例数（0 表示全部通过）。
int RunAll()
{
    std::vector<TestCase>& registry = Registry();
    int nPass = 0;
    int nFail = 0;
    for (size_t i = 0; i < registry.size(); ++i)
    {
        std::printf("[ RUN  ] %s\n", registry[i].strName);
        try
        {
            registry[i].fnBody();
            std::printf("[ PASS ] %s\n", registry[i].strName);
            ++nPass;
        }
        catch (...)
        {
            std::printf("[ FAIL ] %s\n", registry[i].strName);
            ++nFail;
        }
    }
    std::printf("========================================\n");
    std::printf("total=%zu  pass=%d  fail=%d\n", registry.size(), nPass, nFail);
    return nFail;
}

} // namespace testfw
