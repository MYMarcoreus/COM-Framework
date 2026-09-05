# DataHub · Framework（HTTP 框架层，web:: 命名空间）

DataHub 的**通用 Web 框架层**：与 DataHub 业务解耦、只依赖 Sogou Workflow，
可连同本目录整体移植到其它基于 Workflow 的服务器项目复用。

## 组件清单

| 文件 | 组件 | 职责 |
|---|---|---|
| `HttpMessage.h/.cpp` | `web::CHttpRequest` / `CHttpResponse` | 请求/响应薄封装：方法、路径、头、body、来源、query 参数、Content-Length、路径参数；写 JSON / 文本 / 文件（图片内联 + Range 206 分段）、自定义头、状态码读取 |
| `HttpRouter.h/.cpp` | `web::CHttpRouter` | 路径模板路由：`/api/text/{id}` 捕获段、方法匹配、注册/分发；线程安全（注册与分发可跨线程） |
| `HttpText.h/.cpp` | `web::CHttpText` | 纯文本工具（无 workflow 依赖）：URL / HTML / JSON 编解码、非 ASCII 判断、MIME 类型、磁盘文件读取 |

## 设计约定

- **业务不接触 workflow 类型**：框架层把 `WFHttpTask` 收口在
  `HttpMessage` 内，业务/控制器只看到 `CHttpRequest / CHttpResponse`。
- **写大文件用 Range**：`CHttpResponse::WriteFile` 支持 `Range` 分段（206），
  用于规避 workflow 在部分网络路径上对单次超大响应体的截断问题；
  客户端应按 64KB 分段拉取再拼接。
- **JSON 由业务拼装**，但字符串成员务必经 `CHttpText::JsonString` 转义
  （勿用 `HtmlEscape`，二者语义不同）。

## 移植到其它 Workflow 项目

1. 拷贝本目录到目标项目（如 `Xxx/Framework/`）；
2. 编译加入 `HttpMessage.cpp / HttpRouter.cpp / HttpText.cpp`；
3. 头文件搜索路径保证可 `#include "Framework/HttpXxx.h"`；
4. 业务控制器类 `RegisterRoutes(CHttpRouter&)` 注册路由，
   装配层把控制器注册进同一个 router 后挂到 `WFHttpServer` 的 process。

> 本层不含任何 DataHub 业务；业务在 `../Module/` 各模块目录。
