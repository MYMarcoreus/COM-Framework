// ============================================================================
// WorkflowDemo demo_parallel —— 并行 HTTP 请求示例
//
// 用法:
//   ./build/WorkflowDemo/demo_parallel <url1> <url2> <url3> ...
//
// 展示 workflow 并行任务流能力：
//   - ParallelWork 并行抓取多个 URL
//   - 每个子任务把结果存入 series context
//   - 所有子任务完成后触发并行回调，从 context 汇总打印
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/Workflow.h"
#include "workflow/WFFacilities.h"

static WFFacilities::WaitGroup wait_group(1);

// 每个子任务的上下文：URL + 请求结果
struct ParallelContext
{
    std::string url;
    int state;
    int error;
    protocol::HttpResponse resp;
};

// ----------------------------------------------------------------------------
// 单个子请求回调：把结果写入 series context
// ----------------------------------------------------------------------------
void SubTaskCallback(WFHttpTask* task)
{
    ParallelContext* ctx =
        static_cast<ParallelContext*>(series_of(task)->get_context());
    ctx->state = task->get_state();
    ctx->error = task->get_error();
    ctx->resp = std::move(*task->get_resp());
}

// ----------------------------------------------------------------------------
// 并行回调：所有子任务完成后触发，从 context 汇总打印
// ----------------------------------------------------------------------------
void ParallelCallback(const ParallelWork* pwork)
{
    size_t n = pwork->size();
    printf("[all] %zu parallel requests finished\n", n);

    size_t i = 0;
    for (const SeriesWork* series : *pwork)
    {
        ParallelContext* ctx = static_cast<ParallelContext*>(series->get_context());
        if (ctx->state == WFT_STATE_SUCCESS)
        {
            const void* body = NULL;
            size_t len = 0;
            ctx->resp.get_parsed_body(&body, &len);
            printf("[%zu] %s -> %s %s (body %zu bytes)\n",
                   i++, ctx->url.c_str(),
                   ctx->resp.get_status_code(),
                   ctx->resp.get_reason_phrase(), len);
        }
        else
        {
            printf("[%zu] %s -> failed (state=%d error=%d)\n",
                   i++, ctx->url.c_str(), ctx->state, ctx->error);
        }
        delete ctx;
    }
    wait_group.done();
}

// ----------------------------------------------------------------------------
// 主函数：并行抓取所有 URL
// ----------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s <url1> <url2> [<url3> ...]\n", argv[0]);
        return 1;
    }

    // 创建并行工作流
    ParallelWork* pwork = Workflow::create_parallel_work(ParallelCallback);

    for (int i = 1; i < argc; ++i)
    {
        // 创建 HTTP 请求任务（5 次重定向，2 次重试）
        WFHttpTask* task = WFTaskFactory::create_http_task(
            argv[i], 5, 2, SubTaskCallback);

        // 每个任务包成一个 series，并挂上独立 context
        SeriesWork* series = Workflow::create_series_work(task, NULL);
        ParallelContext* ctx = new ParallelContext;
        ctx->url = argv[i];
        series->set_context(ctx);
        pwork->add_series(series);
    }

    pwork->start();
    wait_group.wait();
    return 0;
}
