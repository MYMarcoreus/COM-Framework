# 扩展指南 — 使用文档

> 实现机制见：[extensibility-impl.md](extensibility-impl.md)

## 1. 新增一个模块

1. 在 ServerCore 或业务项目下新建模块目录（头源同目录）；
2. 定义接口（`I` 前缀）+ `IID_XXX()`，或复用已有接口；
3. 实现 `C*Module`：继承 `CModule`，必要时实现接口、重写生命周期；
4. 构造中 `AddDependency` 声明硬依赖；`Initialize(ctx)` 解析依赖；
5. 在 Application 的 `RegisterModules()` 注册。

```text
MyModule/
├── IMyInterface.h      # 接口 + IID
├── MyModule.h          # 实现
└── MyModule.cpp
```

## 2. 新增一个服务器

1. 复制骨架项目（参考 ServerA / LogServer）；
2. `Application/` 下写 `C*Application : sc::CMyApplication`；
3. `RegisterModules()`：默认装配 → 注册网络 / 事件 / 消息路由 → 业务模块；
4. `main.cpp`：`Initialize → Start → Run → Shutdown`；
5. 编写协议提取器与业务服务。

## 3. 新增一个协议

1. 定义命令枚举（`kCmdXxx`）；
2. 实现 `ParsePacket`（或直接实现 `MakeMessageExtractor()`）；
3. 在业务服务 `Initialize` 中：`router->SetExtractor(...)` + `router->RegisterHandler(kCmdXxx, handler)`。

### 提取器要点

- 直接解析输入缓冲，**零拷贝**（负载借用缓冲内部指针）；
- 半包返回 `kNeedMore`，非法返回 `kInvalid`；
- 限制最大长度防止恶意超长报文。

## 4. 组合根装配模板

```cpp
bool CMyApp::RegisterModules()
{
    if (!CMyApplication::RegisterModules()) return false; // IConfig/ILogger/IMetrics

    // 基础设施（先注册，保证先初始化）
    if (!m_moduleManager.RegisterModule(sc::IID_IAsyncExecutor(), new sc::CAsyncExecutorModule(2))) return false;
    if (!m_moduleManager.RegisterModule(sc::IID_ITimer(), new sc::CTimerModule())) return false;
    if (!m_moduleManager.RegisterModule(sc::IID_INetwork(), new sc::CNetworkModule())) return false;
    if (!m_moduleManager.RegisterModule(sc::IID_IEventDispatcher(), new sc::CEventDispatcher())) return false;
    if (!m_moduleManager.RegisterModule(sc::IID_IMessageRouter(), new sc::CMessageRouter())) return false;

    // 业务模块
    if (!m_moduleManager.RegisterModule(sc::IID_INetworkHandler(), new CMyService())) return false;
    return true;
}
```

## 5. 约定

- 模块名进程内唯一（`GetName()`），用于管理与日志；
- 硬依赖用 `AddDependency`（拓扑排序），可选依赖靠注册顺序；
- 异步 / 回调任务必须捕获 `Self()` 自持引用；
- 网络回调尽快返回，重活投递 `IAsyncExecutor`；
- 指标命名 `<模块>.<量名>`，通过 `IMetrics` 上报；
- 协议与业务逻辑属于业务项目，不放入 ServerCore。
