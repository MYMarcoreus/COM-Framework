// ============================================================================
// WorkflowDemo demo_client —— HTTP 客户端示例（类似 wget）
//
// 用法:
//   ./build/WorkflowDemo/demo_client <url> [<url> ...]
//
// 展示 workflow 客户端能力：
//   - WFTaskFactory::create_http_task 创建 HTTP 请求任务
//   - 回调中检查状态（WFT_STATE_*）并打印响应
//   - series 串行流程：抓取所有 URL
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/WFFacilities.h"

// 全局等待组指针（main 中延迟构造，回调中 done() 递减）
static WFFacilities::WaitGroup* g_wait_group = NULL;

// ----------------------------------------------------------------------------
// HTTP 响应回调
// ----------------------------------------------------------------------------
void HttpCallback(WFHttpTask* task)
{
    protocol::HttpResponse* resp = task->get_resp();
    int state = task->get_state();
    int error = task->get_error();

    // 打印请求 URL
    printf("URL: %s\n", task->get_req()->get_request_uri());

    // 状态检查
    switch (state)
    {
    case WFT_STATE_SYS_ERROR:
        printf("  system error: %s\n", strerror(error));
        break;
    case WFT_STATE_DNS_ERROR:
        printf("  DNS error: %s\n", gai_strerror(error));
        break;
    case WFT_STATE_TASK_ERROR:
        printf("  task error: %d\n", error);
        break;
    case WFT_STATE_SUCCESS:
        break;
    default:
        break;
    }

    if (state != WFT_STATE_SUCCESS)
    {
        printf("  request failed\n");
        g_wait_group->done();
        return;
    }

    // 状态行
    printf("  %s %s %s\n", resp->get_http_version(),
           resp->get_status_code(), resp->get_reason_phrase());

    // 响应头
    protocol::HttpHeaderCursor cursor(resp);
    std::string name;
    std::string value;
    while (cursor.next(name, value))
        printf("  %s: %s\n", name.c_str(), value.c_str());

    // 响应体
    const void* body = NULL;
    size_t body_len = 0;
    resp->get_parsed_body(&body, &body_len);
    if (body && body_len > 0)
    {
        printf("  --- body (%zu bytes) ---\n", body_len);
        fwrite(body, 1, body_len, stdout);
        printf("\n");
    }
    g_wait_group->done();
}

// ----------------------------------------------------------------------------
// 主函数：逐个抓取 URL
// ----------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <url> [<url> ...]\n", argv[0]);
        return 1;
    }

    // 等待组计数 = 请求数（构造时指定，回调中 done() 递减）
    g_wait_group = new WFFacilities::WaitGroup(argc - 1);

    for (int i = 1; i < argc; ++i)
    {
        // 创建 HTTP GET 请求任务（0 重试，1 次重定向，超时 10s）
        WFHttpTask* task = WFTaskFactory::create_http_task(
            argv[i], 0, 1, HttpCallback);
        task->start();
    }

    // 等待所有任务完成
    g_wait_group->wait();
    delete g_wait_group;
    return 0;
}
