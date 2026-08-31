# WorkflowDemo —— Sogou Workflow 使用示例

一个演示如何使用 [Sogou Workflow](https://github.com/sogou/workflow)（`nossl` 分支）的独立示例项目。

## 说明

- 独立示例项目，不依赖 ServerCore/Common，仅链接第三方库 `libworkflow.a`。
- workflow 为 git 子模块（`ThirdParty/workflow`），静态库由 `ThirdParty/build.sh` 独立编译。
- 使用 `nossl` 分支：移除了 SSL 协议实现（无 `libssl` 依赖），但 HTTP 内部仍用 OpenSSL crypto 做 base64，链接需 `-lcrypto -lpthread`。

## 构建

```bash
# 1. 初始化子模块（首次）
git submodule update --init --recursive

# 2. 编译 workflow 静态库（libworkflow.a）
./ThirdParty/build.sh workflow

# 3. 构建本示例（Makefile 缺失 libworkflow.a 时会自动调用第 2 步）
./build.sh WorkflowDemo
```

## 运行

```bash
# ① HTTP echo 服务器（默认 8888 端口）
./build/release/demo_server 8888

# ② HTTP 客户端（抓取 URL，等价 wget）
./build/release/demo_client http://127.0.0.1:8888/hello

# ③ 并行请求（同时抓取多个 URL）
./build/release/demo_parallel http://127.0.0.1:8888/a http://127.0.0.1:8888/b http://127.0.0.1:8888/c

# ④ 复杂任务流（图 DAG：timer → 并行 http → go 汇总）
./build/release/demo_graph http://127.0.0.1:8888/a http://127.0.0.1:8888/b http://127.0.0.1:8888/c
```

## 示例内容

| 文件 | 演示能力 |
| --- | --- |
| `main.cpp` | 程序入口（解析端口、启动服务器） |
| `HttpEchoServer.h/.cpp` | `CHttpEchoServer`：封装 `WFHttpServer`，echo 请求行/头/体 |
| `demo_client.cpp` | `WFTaskFactory::create_http_task` 客户端（wget 风格） |
| `demo_parallel.cpp` | `Workflow::create_parallel_work` 并行任务流 |
| `demo_graph.cpp` | `WFGraphTask` 图 DAG：定时器 + 并行 HTTP + go 计算 + 依赖边（`a-->b`） |

## 关键头文件

```cpp
#include "workflow/WFHttpServer.h"   // 服务端
#include "workflow/WFTaskFactory.h"  // 客户端任务工厂
#include "workflow/WFFacilities.h"   // WaitGroup 同步
#include "workflow/HttpMessage.h"    // 请求/响应协议
```
