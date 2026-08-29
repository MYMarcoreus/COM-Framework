/// @file main.cpp
/// 测试入口。
///   ./tests                        —— 运行全部单元测试（含汇总表格）
///   ./tests --benchmark            —— 运行基准测试（与基准文件比较）
///   ./tests --update-benchmark     —— 运行基准测试并更新基准文件

#include <cstring>

#include "TestFramework.h"
#include "Benchmark.h"

int main(int argc, char** argv)
{
    bool bBench = false;
    bool bUpdate = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--benchmark") == 0)
        {
            bBench = true;
        }
        else if (std::strcmp(argv[i], "--update-benchmark") == 0)
        {
            bBench = true;
            bUpdate = true;
        }
    }

    if (bBench)
    {
        return bench::RunAll(bUpdate);
    }
    return testfw::RunAll();
}
