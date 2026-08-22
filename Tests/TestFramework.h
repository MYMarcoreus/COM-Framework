#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testfw {

/// @brief 测试用例。
struct TestCase
{
    const char* strName;
    std::function<void()> fnBody;
};

/// @brief 全局用例注册表。
std::vector<TestCase>& Registry();

/// @brief 注册测试用例。
void Register(const char* strName, const std::function<void()>& fnBody);

/// @brief 运行全部用例，返回失败数。
int RunAll();

} // namespace testfw

/// @brief 定义测试用例（静态注册，main 前自动注册）。
#define TEST(name)                                                        \
    static void Test_##name();                                            \
    namespace {                                                           \
    struct TestReg_##name                                                 \
    {                                                                     \
        TestReg_##name() { testfw::Register(#name, &Test_##name); }       \
    } g_TestReg_##name;                                                   \
    }                                                                     \
    static void Test_##name()

/// @brief 断言表达式为真。
#define ASSERT_TRUE(expr)                                                 \
    do {                                                                  \
        if (!(expr)) {                                                    \
            std::printf("      FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); \
            throw -1;                                                     \
        }                                                                 \
    } while (0)

/// @brief 断言两值相等。
#define ASSERT_EQ(a, b)                                                   \
    do {                                                                  \
        if (!((a) == (b))) {                                              \
            std::printf("      FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b); \
            throw -1;                                                     \
        }                                                                 \
    } while (0)
