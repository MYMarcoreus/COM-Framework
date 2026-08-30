// ====================================================================
// Benchmark 入口：依次运行各用例，汇总为 markdown 表格。
// 结果写入 Benchmark/results/benchmark-report.md，并打印到终端。
// ====================================================================
#include "cases/ChainCase.h"
#include "cases/CoroutineCase.h"
#include "cases/ResumableCase.h"
#include "cases/StressCase.h"
#include "cases/SubmitCase.h"
#include "framework/Report.h"

int main()
{
    RunSubmitCases();
    RunChainCases();
    RunCoroutineCases();
    RunResumableCases();
    RunStressCases();
    return benchmark::ReportToFiles();
}
