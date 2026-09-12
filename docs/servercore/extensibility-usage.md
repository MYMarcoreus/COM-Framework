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

1. 复制骨架项目（参考 ServerTemplate / ServerExample）；
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
    if (!CMyApplication::RegisterModules()) return false;  // IConfig/ILogger/IMetrics

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

## 6. 模块对外提供异步函数（跨模块异步调用）

业务模块可以对**外**提供多个异步函数，函数内部再调用**本模块内**或**其他模块**的异步函数。
参考实现：`ServerExample/Module/`（`IUserTable` = 模拟数据库的数据访问模块，
`IUserService` = 用户业务模块）。

### 6.1 接口形态

异步函数**返回 promise 句柄**（不是回调）：接口方法是虚函数，返回值是具体类型
`common::async::CPromise<TContext>`（命名对齐 JS Promise），`TContext` 由接口头定义
（纯数据上下文，可默认构造）。

```cpp
inline const sc::InterfaceId& IID_IMyService()
{
    static const sc::InterfaceId iid("myproject::IMyService", "<uuidgen 生成>");
    return iid;
}

class IMyService : public virtual sc::IUnknown
{
   public:
    virtual common::async::CPromise<CMyOpContext> QueryAsync(std::uint64_t id) = 0;  // 异步函数
};
```

- 上下文由模块内部创建并注入 promise，调用方用 `promise.GetContext()` 取结果；
- 调用方用 `Then` / `Catch` / `Finally` / `OnSettled` 接管后续 —— **不需要阻塞等待**；
- 模块实现接口：`SC_DECLARE_INTERFACE_MAP()` + `SC_BEGIN_INTERFACE_MAP` /
  `SC_INTERFACE_ENTRY_EX(IfaceType, IID_XXX())` / `SC_END_INTERFACE_MAP`（自定义接口用 `_EX` 条目）；
- 注册用 `RegisterModule(IID_IMyService(), new CMyModule(...))`，解析用
  `ctx.Resolve<IMyService>(IID_IMyService())`（`AddDependency(IID_IXxx())` 声明硬依赖）。

### 6.2 模块内部编排：本模块内 / 其他模块的异步函数怎么调

| 场景 | 做法 | 说明 |
| --- | --- | --- |
| 本模块内 / 同上下文类型 | 直接串 `Then` / `Catch` / `Finally`（把已有异步函数当构建块复用） | **非阻塞**，不占额外 worker |
| 其他模块（上下文类型不同） | `CPromise<Ctx>::New`（等价 JS `new Promise((resolve, reject) => …)`）桥接对方 promise，再用 `ThenPromise`（等价 JS 的「then 处理器返回 promise 时等待」）接入本流程 | **非阻塞、零协程**：调用方拿到的仍是含跨模块子流程的完整结果 |

跨模块调用的落地写法（完整业务示例见 `ServerExample/Module/ExampleAsyncModule.cpp`）：

```cpp
/// 桥接：把其他模块的 promise 接进本流程（等价 JS 的 new Promise）
common::async::CPromise<CMyOpContext> BridgeQueryOther(const CFlowDeps& deps,
                                                       const std::shared_ptr<CMyOpContext>& spCtx)
{
    return common::async::CPromise<CMyOpContext>::New(
        *deps.spExec, spCtx, [deps, spCtx](const ResolveFn& fnResolve, const RejectFn& fnReject)
    {
        deps.spOther
            ->QueryAsync(spCtx->spOtherOp)  // 其他模块的异步函数（另一套上下文）
            .OnSettled([spCtx, fnResolve, fnReject](common::async::CPromiseResult result)
        {
            if (result.IsRejected())
            {
                fnReject(kMyDbFailed);
                return;
            }                                        // 跨模块拒绝码 → 业务码
            spCtx->nRows = spCtx->spOtherOp->nRows;  // 取回数据
            fnResolve();
        });
    }, ASYNC_LOC);
}

// 本流程：本模块层 → 跨模块桥接 → 本模块层（全程只登记回调，不阻塞任何线程）
common::async::CPromise<CMyOpContext> BuildFlow(const CFlowDeps& deps, const std::shared_ptr<CMyOpContext>& spCtx)
{
    return deps.spExec->NewPromise(spCtx, &StepValidate, ASYNC_LOC)
        .ThenPromise(
            [deps](const std::shared_ptr<CMyOpContext>& sp)
    {
        return BridgeQueryOther(deps, sp);
    }, ASYNC_LOC)
        .Then(&StepUseRows, ASYNC_LOC)
        .Finally(&StepAudit, ASYNC_LOC);  // 收尾：失败也执行，且不改变结果
}
```

要点：

- **每个模块自建执行器**（promise 是模板，无法放进 `IAsyncExecutor` 虚接口）；跨模块调用只登记
  回调，双方线程池互不占用，因此线程数不必为「等待」留余量（示例业务模块只需 2 个 worker）；
- 流程函数应当**按值捕获依赖**（执行器 `shared_ptr`、对方模块接口 `ScopedInterfacePtr`）与上下文，
  **不要在回调里捕获本模块 `this`**：回调可能在别的模块的线程上执行、也可能晚于本模块停止
  （这样写流程就是纯函数，任何线程上都安全）；需要「回调期间模块存活」时用
  `Self<IUserService>()` 之类的自持引用（示例的演示驱动即如此）；
- 桥接处是**唯一**做「跨模块拒绝码 → 业务码」语义转换的地方：把对方语境的「未命中 / 不存在」
  归一化为兑现（业务上不是错误）或映射成对应业务码，`kException` 一并映射并记日志；
- 本模块内复用异步函数时才用 `Then` / `Catch` / `Finally` 串接；`Await()` 是阻塞等待，
  只留给测试与「子流程很短且线程有余量」的场合。
