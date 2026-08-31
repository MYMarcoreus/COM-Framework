// ============================================================================
// WorkflowDemo demo_graph —— 复杂任务流（图 DAG + 串并联）示例
//
// 用法:
//   ./build/WorkflowDemo/demo_graph
//
// 演示 workflow 复杂任务编排能力：
//   - WFGraphTask 图任务：用 a-->b 表达任意 DAG 依赖
//   - 混合节点：HTTP 请求 / 定时器 / go 计算任务
//   - 串并联混合：节点间并行 + 依赖串联
//
// 流程（DAG）:
//   timer(1s) ─┬─> http(url1) ─┐
//              ├─> http(url2) ─┼─> go(合并统计) ─> 打印汇总
//              └─> http(url3) ─┘
// ============================================================================
#include <stdio.h>
#include <string>
#include <vector>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/WFGraphTask.h"
#include "workflow/WFFacilities.h"

static WFFacilities::WaitGroup wait_group(1);

// 待抓取的 URL（固定写死，便于演示）
static const char* kUrls[] = {
    "http://127.0.0.1:8888/a",
    "http://127.0.0.1:8888/b",
    "http://127.0.0.1:8888/c",
};
static const int kUrlCount = sizeof(kUrls) / sizeof(kUrls[0]);

// 各节点共享的数据
struct GraphData
{
    std::vector<std::string> urls;
    size_t sizes[8] = {0};     // 每页 body 大小
    int ok_count = 0;          // 成功抓取的页数
};

// ----------------------------------------------------------------------------
// HTTP 请求节点：抓取一个 URL，把 body 大小写入共享数据
// ----------------------------------------------------------------------------
void FetchHttp(WFHttpTask* task, size_t idx, GraphData* data)
{
    if (task->get_state() == WFT_STATE_SUCCESS)
    {
        const void* body = NULL;
        size_t len = 0;
        task->get_resp()->get_parsed_body(&body, &len);
        data->sizes[idx] = len;
        data->ok_count++;
        printf("[http %zu] %s -> %s (%zu bytes)\n",
               idx, data->urls[idx].c_str(),
               task->get_resp()->get_status_code(), len);
    }
    else
    {
        data->sizes[idx] = (size_t)-1;
        printf("[http %zu] %s -> FAILED (state=%d)\n",
               idx, data->urls[idx].c_str(), task->get_state());
    }
}

// ----------------------------------------------------------------------------
// 汇总计算节点（go task，在独立线程执行）：统计总字节数
// ----------------------------------------------------------------------------
void Aggregate(GraphData* data)
{
    size_t total = 0;
    int fail = 0;
    for (int i = 0; i < (int)data->urls.size(); ++i)
    {
        if (data->sizes[i] == (size_t)-1)
            fail++;
        else
            total += data->sizes[i];
    }
    printf("[go] aggregate: %d ok, %d fail, total %zu bytes\n",
           data->ok_count, fail, total);
}

// ----------------------------------------------------------------------------
// 主函数：构建并启动复杂任务流
// ----------------------------------------------------------------------------
int main()
{
    GraphData* data = new GraphData;
    for (int i = 0; i < kUrlCount; ++i)
        data->urls.push_back(kUrls[i]);

    // ---- 节点：定时器（先等 1 秒）----
    WFTimerTask* timer = WFTaskFactory::create_timer_task(
        1000000, [](WFTimerTask*) {
            printf("[timer] 1s elapsed\n");
        });

    // ---- 节点：三个 HTTP 请求 ----
    WFHttpTask* http1 = WFTaskFactory::create_http_task(
        data->urls[0], 3, 1, [data](WFHttpTask* task) {
            FetchHttp(task, 0, data);
        });
    WFHttpTask* http2 = WFTaskFactory::create_http_task(
        data->urls[1], 3, 1, [data](WFHttpTask* task) {
            FetchHttp(task, 1, data);
        });
    WFHttpTask* http3 = WFTaskFactory::create_http_task(
        data->urls[2], 3, 1, [data](WFHttpTask* task) {
            FetchHttp(task, 2, data);
        });

    // ---- 节点：汇总计算（go task，独立线程执行）----
    WFGoTask* go = WFTaskFactory::create_go_task(
        "aggregate", Aggregate, data);

    // ---- 构建图 ----
    WFGraphTask* graph = WFTaskFactory::create_graph_task(
        [data](WFGraphTask*) {
            printf("[graph] complete, wakeup main\n");
            delete data;
            wait_group.done();
        });

    // ---- 图节点（node）----
    WFGraphNode& t = graph->create_graph_node(timer);
    WFGraphNode& a = graph->create_graph_node(http1);
    WFGraphNode& b = graph->create_graph_node(http2);
    WFGraphNode& c = graph->create_graph_node(http3);
    WFGraphNode& g = graph->create_graph_node(go);

    // ---- 建边（DAG 依赖）：timer 先，三个 http 并行，最后 go 汇总 ----
    t --> a;   // timer 完成 → http1
    t --> b;   // timer 完成 → http2
    t --> c;   // timer 完成 → http3
    a --> g;   // http1 完成 → go
    b --> g;   // http2 完成 → go
    c --> g;   // http3 完成 → go

    graph->start();
    wait_group.wait();
    return 0;
}
