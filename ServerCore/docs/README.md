# ServerCore 服务器基础框架

ServerCore 是工作区内可直接复用的服务器基础框架（静态库 `libServerCore.a`），
提供模块化装配、依赖注入、消息流水线、网络、事件、可观测性等基础设施。
业务服务器（Demo / ServerA / LogServer）基于它构建，仅需提供协议与业务逻辑。

## 文档索引

| 文档 | 内容 |
|---|---|
| [architecture.md](architecture.md) | 总体架构与分层设计 |
| [module-system.md](module-system.md) | 模块模型：IUnknown / CModule / CModuleManager / 生命周期 |
| [dependency-injection.md](dependency-injection.md) | 依赖注入：CResolveContext / InterfaceIdOf |
| [messaging.md](messaging.md) | 消息流水线：CMessageRouter / 提取器 / 按类型分发 |
| [network.md](network.md) | 网络层：NetworkModule / INetworkHandler / 连接级上下文 |
| [events.md](events.md) | 事件系统：同步 / 异步分发 |
| [observability.md](observability.md) | 可观测性：IMetrics / 状态报告 |
| [serialization.md](serialization.md) | 序列化：CBinaryWriter / CBinaryReader（Common） |
| [extensibility.md](extensibility.md) | 扩展指南：新增模块 / 新服务器 / 新协议 |
| [testing.md](testing.md) | 测试方法：单元测试与端到端验证 |

## 目录结构

```text
ServerCore/
├── Application/      # CMyApplication：生命周期 + 默认装配（IConfig/ILogger/IMetrics）
├── Module/           # 模块模型：IUnknown/InterfaceId/ScopedInterfacePtr + IModule/Module/ModuleManager
│   └── ResolveContext.h # 依赖解析上下文（CResolveContext + InterfaceIdOf）
├── Event/            # IEventDispatcher / CEventDispatcher（同步 + 异步发布）
├── Message/          # IMessageRouter / CMessageRouter（消息流水线）
├── Network/          # INetwork / INetworkHandler / CNetworkModule / CTcpServerModule
├── Infra/            # IConfig/ILogger/ITimer/IThreadPool/IAsyncExecutor + *Module
├── Observability/    # IMetrics / CMetricsModule（统一指标）
├── Process/          # 进程工具
└── Linux/Makefile    # 生成 build/libServerCore.a
```

## 快速上手

```cpp
class CMyServerApp : public sc::CMyApplication
{
protected:
    bool RegisterModules() override
    {
        if (!CMyApplication::RegisterModules()) return false; // 默认装配
        // 注册业务模块 ...
        return true;
    }
};
```

`main.cpp` 中：构造 Application → `Initialize()` → `Start()` → `Run()` → `Shutdown()`。

## 设计原则

- **模块自治**：每个模块一个目录，头源同目录；模块只依赖接口，不依赖装配。
- **组合根集中装配**：Application 的 `RegisterModules()` 集中注册全部模块。
- **依赖注入**：模块在 `Initialize(ctx)` 中按类型解析依赖，不持有管理器引用。
- **基础设施下沉**：缓冲 / 半包 / 粘包 / 指标 / 事件等通用能力在 ServerCore，业务只写协议与逻辑。
