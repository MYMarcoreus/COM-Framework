---

name: server-core
description: ServerCore 服务器基础框架开发规范。创建、修改、扩展 ServerCore、MyApplication、基础组件、组件模型、模块接口、通信基础设施或 Demo Server 时使用。ServerCore 必须提供可直接被其他服务器项目复用的基础组件，并且初始版本必须包含一个可以运行的最小 Demo。
------------------------------------------------------------------------------------------------------------------------------------------------------------------------

# ServerCore 服务器基础框架规范

## 1. ServerCore 的定位

`ServerCore` 是整个后端项目的**服务器基础框架和公共组件库**。

ServerCore 的目标不是实现某一个具体服务器，而是：

> 提供一组可以被其他服务器项目直接引用的、独立、可复用、低耦合的服务器基础组件。

其他服务器项目应该建立在 ServerCore 之上，而不是复制 ServerCore 中的代码。

总体关系：

```text
                    ServerCore
                        │
        ┌───────────────┼────────────────┐
        │               │                │
        ▼               ▼                ▼
   Application      Component        Communication
    Runtime           Model             Base
        │               │                │
        └───────────────┼────────────────┘
                        │
                        ▼
                Other Server Projects
                        │
        ┌───────────────┼────────────────┐
        ▼               ▼                ▼
   Protocol          Business         Service
    Module            Module          Module
```

---

# 2. ServerCore 不负责什么

ServerCore **不应该包含具体服务器的业务代码**。

尤其不要将以下内容直接放入 ServerCore：

* 具体通信协议
* Protobuf 消息定义
* HTTP/RPC 具体协议
* 登录协议
* 业务消息
* 用户管理
* 权限业务
* 数据库业务
* Redis 业务
* 具体服务器逻辑
* 某个服务器专用的 Service
* 某个业务模块的 Controller

例如：

```text
LoginProtocol
ChatProtocol
SecurityProtocol
AccountService
DeviceService
UserService
```

都不属于 ServerCore。

---

# 3. 通信协议与 ServerCore 的边界

必须严格区分：

```text
通信基础设施
```

和：

```text
具体通信协议
```

ServerCore 可以提供：

* Socket 封装
* TCP 连接
* UDP Socket
* EventLoop
* IO 多路复用
* Connection
* Acceptor
* Connector
* Buffer
* 网络线程
* 连接生命周期管理
* 基础消息分发机制

但是 ServerCore 不应该提供具体协议。

例如：

```text
ServerCore
├── TcpServer
├── TcpConnection
├── Acceptor
├── Buffer
└── EventLoop
```

而：

```text
SecurityServer
├── SecurityProtocol
├── SecurityMessage
└── SecurityService
```

应该位于具体服务器项目中。

---

# 4. ServerCore 的核心设计目标

ServerCore 必须同时满足：

### 4.1 可复用

其他服务器可以直接引用 ServerCore。

### 4.2 可组合

服务器可以根据需要选择不同组件。

### 4.3 可扩展

可以在不修改 ServerCore 核心代码的情况下增加新的模块。

### 4.4 低耦合

模块之间不能通过具体实现形成大量直接依赖。

### 4.5 可运行

ServerCore 初始版本必须能够支撑一个最小可运行服务器。

### 4.6 可验证

必须提供 Demo 项目验证 ServerCore 的实际使用方式。

---

# 5. ServerCore 初始版本必须可运行

创建 ServerCore 时，不能只创建：

```text
MyApplication.h
MyApplication.cpp
```

然后把其他功能留给以后。

初始版本至少应该提供：

```text
ServerCore
    +
最小通信能力
    +
组件模型
    +
MyApplication
    +
Demo Server
```

最终必须能够：

```text
编译
  ↓
启动 Demo
  ↓
创建 MyApplication
  ↓
初始化 ServerCore
  ↓
启动服务器
  ↓
监听端口
  ↓
接受客户端连接
  ↓
进行最基本的数据通信
  ↓
正常退出
```

Demo 不需要实现真实业务。

Demo 的目的只是证明：

> ServerCore 提供的基础组件确实能够组合成一个可运行的服务器。

---

# 6. 推荐的 ServerCore 目录

ServerCore 应该按照组件职责组织。

初始阶段可以采用：

```text
ServerCore/
├── Include/
│   ├── Application/
│   ├── Component/
│   ├── Network/
│   ├── Thread/
│   ├── Event/
│   ├── Common/
│   └── ...
│
├── Src/
│   ├── Application/
│   ├── Component/
│   ├── Network/
│   ├── Thread/
│   ├── Event/
│   ├── Common/
│   └── ...
│
└── Linux/
    └── Makefile
```

实际目录可以根据组件数量调整。

不要为了目录层次而创建大量空模块。

---

# 7. MyApplication

`MyApplication` 是服务器应用程序的基础运行类。

它的职责是：

* 创建应用运行环境
* 初始化 ServerCore
* 初始化公共组件
* 启动服务器
* 运行主循环
* 处理退出
* 释放资源

它不是业务类。

推荐概念结构：

```text
main()
  │
  ▼
MyApplication
  │
  ├── Initialize()
  │
  ├── Start()
  │
  ├── Run()
  │
  └── Shutdown()
```

最终服务器项目可以采用类似：

```cpp
int main()
{
    MyApplication app;

    if (!app.Initialize())
    {
        return -1;
    }

    return app.Run();
}
```

具体接口根据最终组件模型确定。

---

# 8. MyApplication 必须支持组件组合

MyApplication 不应该硬编码所有 ServerCore 模块。

推荐：

```text
MyApplication
      │
      ├── Component A
      ├── Component B
      ├── Component C
      └── Component D
```

应用程序根据实际需要决定加载哪些组件。

例如：

```text
DemoServer
    │
    └── MyApplication
          │
          ├── Logger
          ├── Network
          ├── Timer
          └── EventLoop
```

另一个服务器可能只使用：

```text
SecurityServer
    │
    └── MyApplication
          │
          ├── Logger
          ├── Network
          ├── Database
          └── Timer
```

ServerCore 不应该要求所有服务器必须使用全部组件。

---

# 9. 组件模型

ServerCore 的核心功能之一是提供**组件化架构**。

组件之间不应该依赖具体实现，而应该通过抽象接口进行通信。

核心思想参考 COM：

```text
Component
    │
    ├── Interface
    │
    ├── QueryInterface
    │
    ├── Reference Counting
    │
    └── Lifetime Management
```

这里借鉴的是 COM 的**组件模型思想**，而不是要求项目直接依赖 Windows COM。

ServerCore 必须保持 Linux/Ubuntu 环境下可用。

---

# 10. COM 思想

ServerCore 的组件模型应该具备以下思想：

### 10.1 接口与实现分离

调用方只依赖接口：

```text
Interface
    ↑
    │
Implementation
```

而不是：

```text
Caller
    ↓
ConcreteImplementation
```

---

### 10.2 组件独立

每个组件具有明确职责。

例如：

```text
ILogger
INetwork
ITimer
IConfig
IThread
```

调用方通过接口使用它们。

---

### 10.3 接口查询

组件应该能够根据接口标识获取指定接口。

概念上：

```cpp
void* QueryInterface(const InterfaceId& iid);
```

具体接口和返回类型需要根据实际设计进一步确定。

---

### 10.4 生命周期管理

组件必须具有明确的生命周期管理方式。

应避免：

```text
组件 A 创建组件 B
组件 C 删除组件 B
组件 D 又持有组件 B
```

这种不明确的所有权关系。

组件模型应统一规定：

* 创建
* 引用
* 释放
* 销毁

---

# 11. 接口标识

如果采用 COM 风格组件模型，应设计统一的接口标识。

例如：

```text
InterfaceId
```

用于唯一标识接口。

概念：

```text
IID_ILogger
IID_INetwork
IID_ITimer
IID_IConfig
```

具体实现可以使用：

* GUID
* UUID
* 其他稳定的接口标识

具体方案需要在实现组件模型时统一确定。

不要让每个模块自行设计一套接口标识方案。

---

# 12. 引用计数

如果采用 COM 风格的生命周期管理，可以考虑：

```cpp
AddRef();
Release();
```

其基本思想是：

```text
创建组件
    ↓
Reference Count = 1
    ↓
AddRef()
    ↓
Reference Count++
    ↓
Release()
    ↓
Reference Count--
    ↓
Reference Count == 0
    ↓
Destroy
```

必须保证引用计数操作是线程安全的，如果组件允许跨线程使用。

不要在没有明确所有权模型的情况下混合：

```text
delete
Release()
shared_ptr
```

---

# 13. IUnknown 风格接口

如果最终确定采用 COM 风格模型，可以考虑设计类似：

```cpp
class IUnknown
{
public:
    virtual Result QueryInterface(
        const InterfaceId& iid,
        void** object) = 0;

    virtual unsigned long AddRef() = 0;

    virtual unsigned long Release() = 0;

protected:
    virtual ~IUnknown() {}
};
```

这里只作为设计方向。

不要直接复制 Windows `IUnknown` 的实现。

最终接口必须根据本项目的 C++11、Linux 和实际需求重新设计。

---

# 14. 组件注册

如果组件需要动态发现，可以提供组件注册机制。

例如：

```text
ComponentManager
        │
        ├── RegisterComponent()
        ├── CreateComponent()
        ├── GetComponent()
        └── RemoveComponent()
```

概念关系：

```text
Application
      │
      ▼
ComponentManager
      │
      ├── Logger
      ├── Network
      ├── Timer
      └── Config
```

组件管理器属于 ServerCore。

具体业务组件不应修改 ComponentManager 的核心实现。

---

# 15. 模块之间的通信

模块之间优先使用接口，而不是直接访问对方实现。

不推荐：

```text
Network.cpp
    ↓
直接创建
    ↓
具体业务 Service
```

推荐：

```text
Network
    ↓
Interface
    ↓
Service
```

这样可以替换具体实现。

---

# 16. ServerCore 组件分类

初期建议按照以下方向逐步建设：

```text
ServerCore
│
├── Application
│   └── MyApplication
│
├── Component
│   ├── Component
│   ├── Interface
│   ├── ComponentManager
│   └── Reference Management
│
├── Network
│   ├── Socket
│   ├── Acceptor
│   ├── Connection
│   ├── Buffer
│   └── EventLoop
│
├── Thread
│   ├── Thread
│   ├── Mutex
│   └── Synchronization
│
├── Event
│   └── Event Dispatcher
│
├── Timer
│   └── Timer Manager
│
├── Log
│   └── Logger
│
└── Common
    └── Common Utilities
```

不要求第一次实现全部模块。

应根据实际需求逐步增加。

---

# 17. 通信模块的边界

ServerCore 的 Network 模块只负责：

```text
Socket
Connection
IO
EventLoop
Buffer
```

不负责：

```text
Protocol
Message Definition
Business Message
Command
RPC Message
```

例如：

```text
ServerCore
└── Network
    ├── TcpServer
    ├── TcpConnection
    ├── Acceptor
    ├── Buffer
    └── EventLoop
```

具体服务器：

```text
DemoServer
├── Protocol
│   ├── Message
│   └── Packet
│
├── Service
└── main.cpp
```

---

# 18. Demo 项目

ServerCore 必须提供一个 Demo 项目。

推荐：

```text
Demo/
├── Src/
├── Include/
└── Linux/
    └── Makefile
```

Demo 不属于 ServerCore 本身的核心组件。

Demo 是 ServerCore 的**验证项目**。

---

# 19. Demo 的职责

Demo 必须证明以下能力：

### 19.1 可以创建 MyApplication

```text
main
 ↓
MyApplication
```

### 19.2 可以初始化 ServerCore

```text
MyApplication
 ↓
ComponentManager
 ↓
基础组件
```

### 19.3 可以启动服务器

至少能够：

```text
创建 Socket
 ↓
Bind
 ↓
Listen
 ↓
Accept
```

---

# 20. Demo 通信

Demo 应提供一个极简通信协议。

但是：

> **该协议不能放入 ServerCore。**

协议应该属于 Demo：

```text
Demo/
├── Protocol/
├── Src/
└── Include/
```

或者根据项目目录规范：

```text
Demo/
├── Include/
│   └── Protocol/
├── Src/
│   └── Protocol/
└── Linux/
```

协议只用于验证：

```text
ServerCore Network
        ↓
接收数据
        ↓
Demo Protocol
        ↓
解析
        ↓
Demo Service
```

---

# 21. Demo 协议

Demo 协议应保持极简。

例如：

```text
+----------+----------+----------+
| Length   | Command  | Payload  |
+----------+----------+----------+
```

协议可以支持：

```text
PING
PONG
```

或者：

```text
HELLO
ECHO
```

重点不是协议功能，而是验证：

```text
网络连接
 ↓
数据接收
 ↓
协议解析
 ↓
组件调用
 ↓
数据发送
```

---

# 22. ServerCore 与 Demo 的关系

正确关系：

```text
                    ServerCore
                        │
          ┌─────────────┼─────────────┐
          │             │             │
          ▼             ▼             ▼
      Component      Network       Application
          │             │             │
          └─────────────┼─────────────┘
                        │
                        ▼
                      Demo
                        │
              ┌─────────┼─────────┐
              ▼         ▼         ▼
           Protocol   Service    main
```

Demo 可以依赖 ServerCore。

ServerCore 不允许依赖 Demo。

---

# 23. ServerCore 的静态库

ServerCore 应作为独立公共库构建：

```text
libServerCore.a
```

其他服务器：

```text
ServerA
    ↓
libServerCore.a
```

```text
ServerB
    ↓
libServerCore.a
```

```text
Demo
    ↓
libServerCore.a
```

---

# 24. 依赖方向

严格遵循：

```text
Demo / Server
       ↓
 ServerCore
       ↓
Common Infrastructure
       ↓
Third-party / POSIX
```

禁止：

```text
ServerCore
    ↓
Demo
```

禁止：

```text
ServerCore
    ↓
具体业务服务器
```

---

# 25. 第三方库

ServerCore 使用的第三方库必须遵循项目统一的依赖管理规范。

优先选择：

* 轻量
* C++11 兼容
* Linux 兼容
* 支持静态编译
* 依赖少
* 容易维护

第三方源码统一放在公共目录。

编译成：

```text
libxxx.a
```

ServerCore 链接这些静态库。

---

# 26. 第一阶段建议实现

第一次创建 ServerCore 时，建议不要一次实现全部功能。

第一阶段至少完成：

```text
ServerCore
│
├── Application
│   └── MyApplication
│
├── Component
│   ├── Interface
│   ├── Component
│   └── ComponentManager
│
└── Network
    ├── Socket
    ├── TcpServer
    ├── TcpConnection
    └── Buffer

Demo
│
├── Protocol
├── Service
└── main
```

最终实现：

```text
启动 Demo
    ↓
MyApplication
    ↓
初始化组件
    ↓
启动 TcpServer
    ↓
监听端口
    ↓
Accept
    ↓
TcpConnection
    ↓
接收 Demo Protocol
    ↓
解析
    ↓
Service
    ↓
返回响应
```

---

# 27. 第一阶段不应该实现

不要在第一阶段加入：

* 数据库
* Redis
* RPC
* ZooKeeper
* HTTP
* HTTPS
* 复杂配置中心
* 服务发现
* 消息队列
* 分布式事务
* 业务认证
* 权限系统

这些属于后续需求。

---

# 28. 设计原则

ServerCore 的设计遵循：

```text
接口优先
    ↓
组件化
    ↓
低耦合
    ↓
可组合
    ↓
可复用
    ↓
可扩展
```

而不是：

```text
先实现一个巨大 Server
        ↓
再拆模块
```

---

# 29. 不要过度设计

COM 思想用于解决：

* 接口隔离
* 组件生命周期
* 模块解耦
* 接口查询
* 组件复用

不能为了“像 COM”而复制 Windows COM 的全部复杂机制。

不要无需求地实现：

* 注册表
* COM Proxy/Stub
* OLE
* DCOM
* RPC Runtime
* 二进制类型库
* Windows COM 激活机制

本项目只借鉴 COM 的组件模型思想。

---

# 30. 组件设计检查清单

新增 ServerCore 组件时必须回答：

1. 这个组件属于基础设施还是业务？
2. 是否应该由 ServerCore 提供？
3. 是否存在抽象接口？
4. 谁创建组件？
5. 谁拥有组件？
6. 谁释放组件？
7. 是否需要引用计数？
8. 是否需要 `QueryInterface`？
9. 是否允许跨线程访问？
10. 是否依赖具体业务模块？
11. 是否能够被其他服务器复用？
12. 是否需要第三方库？
13. 是否能够独立测试？
14. 是否需要加入 Demo 验证？

如果无法明确回答上述问题，不要直接实现组件。

---

# 31. 新增 ServerCore 模块的流程

新增模块时：

```text
确定职责
   ↓
确定是否属于 ServerCore
   ↓
设计接口
   ↓
确定生命周期
   ↓
确定组件依赖
   ↓
实现接口
   ↓
实现具体组件
   ↓
加入 ComponentManager（如果需要）
   ↓
加入 Makefile
   ↓
编译 libServerCore.a
   ↓
Demo 验证
   ↓
再提供给其他服务器使用
```

---

# 32. 验证原则

ServerCore 的任何核心能力都应该优先通过 Demo 验证。

例如增加 Timer：

```text
Timer
 ↓
Demo
 ↓
创建 Timer
 ↓
触发回调
 ↓
验证
```

增加 Network：

```text
Network
 ↓
Demo
 ↓
启动服务器
 ↓
客户端连接
 ↓
发送数据
 ↓
服务器响应
```

增加 Component：

```text
Component
 ↓
Demo
 ↓
注册
 ↓
查询接口
 ↓
调用
 ↓
Release
```

不能只编译 ServerCore，而不验证实际使用方式。

---

# 33. 最终目标

最终 ServerCore 应形成：

```text
                         ServerCore
                             │
        ┌────────────────────┼────────────────────┐
        │                    │                    │
        ▼                    ▼                    ▼
   Application          Component             Network
        │                    │                    │
        ▼                    ▼                    ▼
 MyApplication         ComponentManager       TcpServer
                                             TcpConnection
        │                    │                    │
        └────────────────────┼────────────────────┘
                             │
                             ▼
                      Server Applications
                             │
              ┌──────────────┼──────────────┐
              ▼              ▼              ▼
           DemoServer     SecurityServer   OtherServer
              │              │              │
              ▼              ▼              ▼
          Protocol         Protocol        Protocol
          Service          Service         Service
```

核心原则：

> **ServerCore 提供“服务器怎么运行”的能力；具体服务器项目提供“服务器运行什么业务”的能力。**

ServerCore 应该是基础设施，而不是业务项目。

ServerCore 应该可以独立编译成 `libServerCore.a`。

ServerCore 初始版本必须提供可运行的 `MyApplication` 和 Demo。

Demo 必须证明 ServerCore 的组件模型和基础通信能力能够实际工作。

具体通信协议属于 Demo 或其他服务器项目，不属于 ServerCore。

模块之间通过接口和组件模型进行组合，组件模型借鉴 COM 的接口、查询、引用计数和生命周期管理思想，但不直接依赖 Windows COM。
