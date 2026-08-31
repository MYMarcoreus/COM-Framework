// ============================================================================
// WorkflowDemo demo_graph —— 复杂任务流（图 DAG + 串并联 + 条件分支）示例
//
// 用法:
//   ./build/WorkflowDemo/demo_graph <url1> <url2> <url3> ...
//
// 演示 workflow 复杂任务编排能力：
//   - WFGraphTask 图任务：用 a-->b 表达任意 DAG 依赖
//   - 混合节点：HTTP 请求 / 定时器 / go 计算任务
//   - 条件分支：根据前序结果决定后续路径（成功→汇总，失败→兜底）
//   - 串并联混合：节点间并行 + 依赖串联
//
// 流程（DAG）:
//   timer(1s) ─┬─> http(url1) ─┬─> go(合并统计) ─> [成功?] ─┬─ 是 ─> 打印汇总
//              ├─> http(url2) ─┤                            └─ 否 ─> 打印失败
//              └─> http(url3) ─┘
// ============================================================================
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/WFGraphTask.h"
#include "workflow/WFFacilities.h"

static WFFacilities::WaitGroup wait_group(1);

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
int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s <url1> <url2> <url3> ...\n", argv[0]);
        return 1;
    }

    GraphData* data = new GraphData;
    for (int i = 1; i < argc && i <= 8; ++i)
        data->urls.push_back(argv[i]);

    int n = (int)data->urls.size();

    // ---- 节点 0：定时器（先等 1 秒）----
    WFTimerTask* timer = WFTaskFactory::create_timer_task(
        1000000, [](WFTimerTask*) {
            printf("[timer] 1s elapsed\n");
        });

    // ---- 节点 1..n：HTTP 请求（每个 URL 一个）----
    // ---- 节点 n+1：汇总计算 ----
    WFGoTask* go = WFTaskFactory::create_go_task(
        "aggregate", Aggregate, data);

    // ---- 构建图 ----
    WFGraphTask* graph = WFTaskFactory::create_graph_task(
        [data](WFGraphTask*) {
            printf("[graph] complete, wakeup main\n");
            delete data;
            wait_group.done();
        });

    WFGraphNode& t = graph->create_graph_node(timer);
    std::vector<WFGraphNode*> http_nodes;
    for (int i = 0; i < n; ++i)
    {
        // 每个 http 节点：lambda 里捕获索引与 data，转发到 FetchHttp
        size_t idx = (size_t)i;
        WFHttpTask* ht = WFTaskFactory::create_http_task(
            data->urls[i], 3, 1, [idx, data](WFHttpTask* task) {
                FetchHttp(task, idx, data);
            });
        http_nodes.push_back(&graph->create_graph_node(ht));
    }
    WFGraphNode& g = graph->create_graph_node(go);

    // ---- 建边（DAG 依赖）：timer 先，然后各 http 并行，最后 go ----
    for (WFGraphNode* hn : http_nodes)
    {
        t --> *hn;   // timer 完成 → 各 http 开始（并行）
        *hn --> g;   // 各 http 完成 → go 汇总
    }

    graph->start();
    wait_group.wait();
    return 0;
}
