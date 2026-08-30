/// @file main.cpp
/// 单元测试入口。
///   ./build/tests    —— 运行全部单元测试
///
/// 说明：基准/压力测试已迁移到独立 Benchmark/ 项目
/// （./build/release/benchmark），旧的 --benchmark / --update-benchmark
/// 入口随 Tests/Benchmark.h 一并移除。
#include "TestFramework.h"

int main()
{
    return testfw::RunAll();
}
