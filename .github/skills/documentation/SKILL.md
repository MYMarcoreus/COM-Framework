---

name: documentation
description: 本项目的 C++ 代码注释、Doxygen 文档、函数分组、代码区域和开发文档规范。创建、修改、重构 C++ 类、函数、接口或 ServerCore 组件时使用。
----------------------------------------------------------------------------------------------

# C++ 代码文档与注释规范

## 1. 总体原则

项目代码注释必须服务于以下目标：

* 说明代码职责。
* 说明复杂逻辑。
* 说明设计原因。
* 说明接口使用方式。
* 方便 Doxygen 生成 API 文档。
* 方便开发者快速浏览代码结构。

注释不得只是机械重复代码。

例如不推荐：

```cpp
// 设置名称
m_Name = name;
```

如果代码本身已经能够明确表达含义，则不需要增加注释。

推荐解释：

```cpp
// 保存应用程序名称，该名称用于日志和启动信息。
m_Name = name;
```

---

# 2. Doxygen 注释统一使用 `///`

项目统一使用：

```cpp
///
```

形式的 Doxygen 注释。

不要在项目代码中混用：

```cpp
/**
 * ...
 */
```

作为主要 Doxygen 风格。

推荐：

```cpp
/// @brief 初始化服务器应用程序。
///
/// @return 成功返回 true，失败返回 false。
bool Initialize();
```

---

# 3. Doxygen 注释的位置

项目明确区分：

```text
头文件 / 类定义
        ↓
简单接口说明

源文件 / 函数实现
        ↓
完整 Doxygen 注释
```

也就是说：

> **函数声明处只保留简单说明，函数定义处提供完整 Doxygen 文档。**

---

# 4. 函数声明处的注释

头文件中的函数声明不需要编写完整 Doxygen。

只需要简单说明函数用途。

例如：

```cpp
class MyApplication
{
public:
    // 初始化应用程序。
    bool Initialize();

    // 启动应用程序。
    bool Start();

    // 运行应用程序主循环。
    int Run();

    // 停止应用程序。
    void Stop();
};
```

不要在头文件中重复写完整的：

```cpp
/// @brief
/// @param
/// @return
```

除非该接口本身是对外公开的核心 API，并且项目文档要求在声明处提供完整接口说明。

默认情况下：

```text
声明处 = 简单说明
实现处 = 完整 Doxygen
```

---

# 5. 函数实现处使用完整 Doxygen

函数定义处使用 `///` 编写完整说明。

例如：

```cpp
/// @brief 初始化服务器应用程序。
///
/// 初始化 ServerCore 所需的基础组件，并建立组件之间的依赖关系。
///
/// @return
///     true  初始化成功。
///     false 初始化失败。
bool MyApplication::Initialize()
{
    ...
}
```

---

# 6. Doxygen 常用标签

根据实际需要使用：

```text
@brief
@param
@return
@note
@warning
@see
```

不要为了格式统一而无意义地添加标签。

---

## 6.1 `@brief`

说明函数最主要的职责。

```cpp
/// @brief 启动 TCP 服务器。
```

---

## 6.2 `@param`

参数存在重要语义时进行说明。

```cpp
/// @param port 服务器监听端口。
```

如果参数名称和作用已经非常明确，可以省略。

---

## 6.3 `@return`

返回值具有多个含义时必须说明。

```cpp
/// @return
///     0  正常退出。
///    -1  初始化失败。
```

---

## 6.4 `@note`

用于说明容易被忽略的重要限制。

```cpp
/// @note 该函数必须在应用程序初始化完成后调用。
```

---

## 6.5 `@warning`

用于描述重要风险。

```cpp
/// @warning 调用该函数前必须确保当前线程已经拥有组件引用。
```

---

## 6.6 `@see`

用于关联重要接口。

```cpp
/// @see Stop()
```

---

# 7. 类注释

类应该提供 Doxygen 类说明。

推荐：

```cpp
/// @brief 服务器应用程序基础类。
///
/// 提供服务器统一的初始化、启动、运行和关闭生命周期。
class MyApplication
{
};
```

如果类的使用存在重要限制，可以补充：

```cpp
/// @note 派生类应该通过重写虚函数扩展服务器生命周期。
```

---

# 8. 构造函数和析构函数

构造函数和析构函数不需要机械添加大量注释。

只有存在特殊生命周期要求时才进行说明。

例如：

```cpp
/// @brief 创建服务器应用程序。
///
/// @param name 应用程序名称。
MyApplication(const std::string& name);
```

如果：

```cpp
MyApplication();
~MyApplication();
```

行为完全明确，可以只在函数声明处使用简单注释。

---

# 9. 函数分组注释

同一类中具有相同职责的一组函数，应使用统一的长分隔注释。

格式：

```cpp
//================ XXXXX ================
```

例如：

```cpp
//================ Lifecycle ================

bool Initialize();

bool Start();

int Run();

void Stop();


//================ Component ================

bool RegisterComponent();

bool RemoveComponent();

void* QueryComponent();


//================ Network ================

bool StartNetwork();

void StopNetwork();
```

---

# 10. 分组标题命名

分组名称应该表达这一组函数的职责。

推荐：

```text
Lifecycle
Component
Network
Timer
Thread
Configuration
Logging
Callback
Message
Connection
Utility
Getter / Setter
```

不要使用没有实际意义的：

```text
Other
Misc
Function
Code
Stuff
```

---

# 11. 分组注释长度

分隔线应该足够明显，使代码可以快速浏览。

统一使用：

```cpp
//================ Lifecycle ================
```

而不要在同一个文件中混用：

```cpp
// Lifecycle
// ----- Lifecycle -----
// ========== Lifecycle ==========
```

项目代码中应保持统一。

---

# 12. Region / EndRegion

如果 VS Code 当前使用的 C++ 插件支持代码折叠区域，可以使用：

```cpp
// #region Lifecycle

...

// #endregion
```

推荐使用：

```cpp
// #region Lifecycle
// #endregion
```

而不是：

```cpp
//Region
//End
```

因为 `#region / #endregion` 是较常见的代码区域标记形式，并且多个编辑器和插件可以识别。

---

# 13. Region 与分组注释配合

对于函数数量较多的类，可以同时使用 Region 和分组标题。

例如：

```cpp
// #region Lifecycle

//================ Lifecycle ================

bool Initialize();

bool Start();

int Run();

void Stop();

// #endregion
```

对于 `.cpp`：

```cpp
// #region Lifecycle

//================ Lifecycle ================

/// @brief 初始化服务器应用程序。
bool MyApplication::Initialize()
{
    ...
}

/// @brief 启动服务器应用程序。
bool MyApplication::Start()
{
    ...
}

/// @brief 运行服务器主循环。
int MyApplication::Run()
{
    ...
}

/// @brief 停止服务器应用程序。
void MyApplication::Stop()
{
    ...
}

// #endregion
```

如果当前编辑器或插件无法识别 `#region`，这些注释仍然只是普通注释，不影响编译。

---

# 14. Region 的使用原则

Region 用于**结构性代码折叠**。

不要把整个文件包在：

```cpp
// #region Everything
```

中。

应该按照职责划分。

例如：

```cpp
// #region Constructor / Destructor

...

// #endregion


// #region Lifecycle

...

// #endregion


// #region Component

...

// #endregion


// #region Network

...

// #endregion
```

---

# 15. 函数内部的步骤注释

对于包含多个明显处理阶段的函数，使用：

```text
①
②
③
④
```

进行步骤说明。

例如：

```cpp
bool MyApplication::Initialize()
{
    // ① 初始化日志系统。
    if (!InitializeLogger())
    {
        return false;
    }

    // ② 创建组件管理器。
    if (!InitializeComponentManager())
    {
        return false;
    }

    // ③ 初始化网络组件。
    if (!InitializeNetwork())
    {
        return false;
    }

    // ④ 启动基础服务。
    return StartComponents();
}
```

---

# 16. 步骤注释的使用场景

只有当函数存在明显的逻辑步骤时，才使用：

```text
① ② ③
```

不要给简单函数强行添加步骤。

不推荐：

```cpp
void SetName(const std::string& name)
{
    // ① 设置名称。
    m_Name = name;
}
```

推荐：

```cpp
void SetName(const std::string& name)
{
    m_Name = name;
}
```

---

# 17. 步骤注释应该解释“为什么”

步骤注释不要机械描述下一行代码。

不推荐：

```cpp
// ① 创建 Socket。
socket();
```

更推荐：

```cpp
// ① 创建监听 Socket，为后续 Bind 和 Listen 做准备。
socket();
```

或者：

```cpp
// ① 创建监听 Socket。
//    此时暂不绑定地址，避免初始化失败时留下未完成的网络资源。
socket();
```

---

# 18. 复杂函数的推荐结构

复杂函数推荐：

```cpp
/// @brief 初始化服务器运行环境。
///
/// 按照日志、组件管理、网络和业务组件的顺序完成初始化。
///
/// @return 初始化成功返回 true，否则返回 false。
bool MyApplication::Initialize()
{
    // ① 初始化日志。
    if (!InitializeLogger())
    {
        return false;
    }

    // ② 初始化组件管理器。
    if (!InitializeComponents())
    {
        return false;
    }

    // ③ 初始化网络。
    if (!InitializeNetwork())
    {
        return false;
    }

    // ④ 启动基础组件。
    if (!StartComponents())
    {
        return false;
    }

    return true;
}
```

这样可以同时满足：

```text
Doxygen
+
代码结构
+
执行步骤
```

---

# 19. 不要滥用注释

以下代码不需要增加注释：

```cpp
int GetPort() const
{
    return m_Port;
}
```

也不需要：

```cpp
// 获取端口。
int GetPort() const
{
    return m_Port;
}
```

如果函数名称已经足够明确，则保持代码简洁。

---

# 20. 注释与代码同步

修改代码逻辑时必须同步检查相关注释。

特别是：

* 函数职责改变。
* 参数含义改变。
* 返回值改变。
* 生命周期改变。
* 线程模型改变。
* 所有权改变。
* 异常/错误处理改变。

禁止出现：

```text
代码已经改变
        ↓
Doxygen 仍然描述旧行为
```

---

# 21. 接口注释

公共接口重点说明：

* 功能
* 参数
* 返回值
* 生命周期
* 所有权
* 线程安全性
* 调用条件
* 失败条件

例如：

```cpp
/// @brief 获取指定接口。
///
/// 根据接口标识查询当前组件支持的接口。
///
/// @param iid 要查询的接口标识。
///
/// @return 借用的接口指针；未找到返回 nullptr（不使用 out 指针）。
///
/// @note 成功获取接口后，调用方需要按照组件的生命周期规则管理引用。
void* QueryInterface(const InterfaceId& iid);
```

---

# 22. 线程安全说明

如果一个公共类或者函数存在明确的线程安全要求，应进行说明。

例如：

```cpp
/// @brief 注册组件。
///
/// 将组件注册到当前组件管理器。
///
/// @note ComponentManager 的注册操作不是线程安全的，应该在应用初始化阶段完成。
bool RegisterComponent(Component* component);
```

或者：

```cpp
/// @note 该函数可以被多个线程并发调用。
```

不要默认声称“线程安全”。

只有代码实际保证线程安全时才能这样描述。

---

# 23. 所有权说明

涉及裸指针、组件接口、引用计数等对象时，应明确所有权。

例如：

```cpp
/// @brief 设置日志组件。
///
/// @param logger 日志组件。
///
/// @note ComponentManager 不取得 logger 的所有权。
void SetLogger(ILogger* logger);
```

或者：

```cpp
/// @brief 创建日志组件。
///
/// @return 返回已经持有一个引用的 ILogger 接口。
///
/// @note 调用方使用完成后必须调用 Release()。
ILogger* CreateLogger();
```

---

# 24. TODO

临时开发内容可以使用：

```cpp
// TODO: 完善组件异常处理。
```

但 TODO 必须描述具体工作。

不推荐：

```cpp
// TODO: 修改。
```

如果 TODO 长期存在，应在正式开发阶段处理，而不是无限保留。

---

# 25. FIXME

存在已知问题但暂时无法解决时使用：

```cpp
// FIXME: 当前实现无法处理组件初始化失败后的回滚。
```

`FIXME` 表示已经确认存在问题，不应该用来表示普通待办事项。

---

# 26. 注释语言

项目代码注释统一使用：

```text
中文
```

API、类名、函数名、变量名等代码标识符保持英文。

例如：

```cpp
/// @brief 初始化网络组件。
///
/// 创建 EventLoop 和 TCP Server，并完成监听端口初始化。
bool InitializeNetwork();
```

---

# 27. 文件级注释

不要求每个 `.cpp` 文件都添加冗长文件头。

如果文件职责不明显，可以使用简短说明：

```cpp
/// @file MyApplication.cpp
/// @brief MyApplication 的生命周期实现。
```

不需要添加作者、创建日期等容易过时的信息，除非项目明确要求。

---

# 28. 注释风格总结

项目统一采用以下层次：

```text
文件
 │
 ├── 文件职责注释（必要时）
 │
 ├── #region
 │
 ├── 函数分组
 │      └── //================ XXXXX ================
 │
 ├── 函数声明
 │      └── 简单注释
 │
 └── 函数实现
        ├── ///
        ├── @brief
        ├── @param
        ├── @return
        └── ① ② ③ 步骤注释
```

---

# 29. 推荐的完整代码风格

头文件：

```cpp
/// @brief 服务器应用程序基础类。
class MyApplication
{
public:

    //================ Constructor / Destructor ================

    // 构造服务器应用程序。
    MyApplication();

    // 析构服务器应用程序。
    virtual ~MyApplication();


    //================ Lifecycle ================

    // 初始化应用程序。
    bool Initialize();

    // 启动应用程序。
    bool Start();

    // 运行应用程序。
    int Run();

    // 停止应用程序。
    void Stop();


    //================ Component ================

    // 获取组件管理器。
    ComponentManager* GetComponentManager();

private:

    ComponentManager* m_ComponentManager;
};
```

源文件：

```cpp
// #region Lifecycle

//================ Lifecycle ================

/// @brief 初始化服务器应用程序。
///
/// 按照基础运行环境、组件管理器和网络组件的顺序完成初始化。
///
/// @return
///     true  初始化成功。
///     false 初始化失败。
bool MyApplication::Initialize()
{
    // ① 初始化基础运行环境。
    if (!InitializeRuntime())
    {
        return false;
    }

    // ② 初始化组件管理器。
    if (!InitializeComponents())
    {
        return false;
    }

    // ③ 初始化网络组件。
    if (!InitializeNetwork())
    {
        return false;
    }

    return true;
}


/// @brief 启动服务器应用程序。
///
/// 启动已经完成初始化的基础组件。
///
/// @return 启动成功返回 true，否则返回 false。
bool MyApplication::Start()
{
    // ① 启动基础组件。
    if (!StartComponents())
    {
        return false;
    }

    // ② 启动网络服务。
    if (!StartNetwork())
    {
        return false;
    }

    return true;
}


/// @brief 运行服务器主循环。
///
/// @return 服务器退出时返回退出码。
int MyApplication::Run()
{
    // ① 确保应用程序已经启动。
    if (!Start())
    {
        return -1;
    }

    // ② 进入服务器主循环。
    m_EventLoop->Run();

    return 0;
}


/// @brief 停止服务器应用程序。
///
/// 按照网络组件、基础组件的顺序停止服务。
void MyApplication::Stop()
{
    // ① 停止网络服务。
    StopNetwork();

    // ② 停止其他基础组件。
    StopComponents();
}

// #endregion
```

---

# 30. 核心原则

本项目的注释系统最终遵循：

```text
简单代码
    ↓
少写注释

复杂函数
    ↓
① ② ③ 步骤注释

函数实现
    ↓
完整 /// Doxygen

函数声明
    ↓
简单说明

同类函数
    ↓
================ Group ================

大量代码区域
    ↓
#region / #endregion

公共 API
    ↓
完整接口、参数、返回值、生命周期和线程安全说明
```

最重要的原则：

> **注释应该帮助开发者理解代码，而不是重复代码。**

> **声明负责快速浏览，实现负责完整文档，分组负责代码结构，①②③负责解释复杂执行流程，Doxygen 负责生成正式 API 文档。**
