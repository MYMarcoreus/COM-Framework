---
name: name-standard
description: 本项目命名规范：以 MFC（Microsoft Foundation Classes）命名风格为基准，MFC 未明确规定的部分采用 Google C++ Style Guide 约定。
---

# 命名规范（name-standard）

## 1. 适用范围

本文档规定本项目（通用 COM 模块化服务器框架）所有 **C++11 代码**的命名规范：

- **基准**：MFC 命名风格（Microsoft Foundation Classes）
- **补充**：MFC 未明确规定处，采用 Google C++ Style Guide

> 说明：现有 Common / ServerCore / Demo / ServerA 中的历史代码风格（如类无 `C` 前缀、成员变量尾下划线 `running_`）不在本轮统一重命名。本文档作为**新代码与后续开发**的命名依据，历史代码是否迁移由项目决策（见 §12）。

---

## 2. 命名空间

- 全小写（Google，MFC 未规定命名空间命名）：
  - `sc`：ServerCore
  - `common`：Common 基础库
  - `demo`：Demo 项目
  - `servera`：ServerA 项目
- 命名空间内标识符不得依赖外部命名空间的未限定符号（用 `using` 或显式限定）。

---

## 3. 文件命名

- **大驼峰法**（PascalCase，每个单词首字母大写）：
  - `TcpServer.h` / `TcpServer.cpp` / `MyApplication.h` / `EventDispatcher.cpp`
- 头文件后缀 `.h`，源文件后缀 `.cpp`（本项目不使用 `.cc`）。
- 文件基名 = 主类型名（类名去 `C` 前缀，接口保留 `I` 前缀）：
  - `CTcpServer` → `TcpServer.h`
  - `CMyApplication` → `MyApplication.h`
  - `INetwork` → `INetwork.h`
  - `CEventDispatcher` → `EventDispatcher.h`
- 结构体 / 别名文件按类型名大驼峰命名（无前缀）：
  - `EventTypes.h`、`NetworkTypes.h`、`Types.h`

---

## 4. 类型命名

### 4.1 类

- MFC：`C` 前缀 + PascalCase（每个单词首字母大写）：
  - `CMyApplication`、`CTcpServer`、`CModuleManager`、`CConfigModule`
- 抽象接口：`I` 前缀（COM / MFC 体系）：
  - `IUnknown`、`INetwork`、`ILogger`、`IModule`、`IEventDispatcher`

### 4.2 结构体

- MFC 中结构体通常无前缀（如 `POINT`、`RECT`）：
  - `Event`、`Packet`、`ConnectionInfo`、`Buffer`

### 4.3 枚举类型

- 类型名 PascalCase（Google 补充）：
  - `LogLevel`、`ModuleState`、`MessageParseResult`

### 4.4 组件 / 模块 / 服务（本项目约定）

- 组件实现：`C` 前缀 + `<能力>Component`：
  - `CNetworkComponent`、`CLoggerComponent`、`CConfigComponent`、`CTimerComponent`
- 模块：`C` 前缀 + `<名称>Module`：
  - `CDemoNetworkModule`、`CDemoTimerModule`、`CServerLoggerModule`
- 服务：`C` 前缀 + `<名称>Service`：
  - `CDemoService`、`CEchoService`

---

## 5. 成员变量

- MFC：`m_` 前缀 + 类型匈牙利前缀 + PascalCase：
  - `m_strName`（字符串）、`m_nCount`（整数）、`m_bRunning`（布尔）、`m_pServer`（指针）、`m_dwFlags`（无符号）
- 常用匈牙利类型前缀：

| 前缀 | 含义 | 示例 |
| --- | --- | --- |
| `str` / `sz` | 字符串 | `m_strHost` |
| `n` | 整数（int/long） | `m_nPort` |
| `b` | 布尔 | `m_bRunning` |
| `p` | 指针 | `m_pConn` |
| `pp` | 二级指针 | `m_ppHandlers` |
| `h` | 句柄 | `m_hSocket` |
| `dw` / `u` | 无符号 | `m_dwFlags` |
| `f` | 浮点 | `m_fTimeout` |
| `t` | 标识（ID） | `m_tTimerId` |

- 示例：

```cpp
class CTcpServer
{
private:
    std::string m_strHost;
    uint16_t m_nPort;
    bool m_bRunning;
    CTcpConnection* m_pConn;
    CTimerId m_tTimerId;
};
```

- 静态成员变量同样使用 `m_` 前缀（MFC 风格统一）。

---

## 6. 成员函数（方法）

- MFC：PascalCase，动词开头：
  - `Start()`、`Stop()`、`GetName()`、`StartTcpServer()`、`QueryInterface()`
- 私有辅助方法：PascalCase（MFC 风格统一，不区分大小写）：
  - `OnCreate()`、`HandleData()`、`DoRead()`、`ShutdownOnIoThread()`
- 获取/设置：`GetXxx()` / `SetXxx()`。

---

## 7. 函数参数

- MFC：匈牙利小写前缀 + PascalCase：
  - `int nPort`、`const std::string& strHost`、`bool bEnable`、`INetworkHandler* pHandler`
- 输出参数：`pOut` / `pp`（指针的指针），**QueryInterface 除外**（采用返回 `void*`，未找到返回 `nullptr`，不用 out 指针）：
  - `void** ppv`（QueryInterface 已废弃此形式）

```cpp
// 接口查询：返回借用的接口指针，未找到返回 nullptr（不用二级指针）。
void* QueryInterface(const InterfaceId& iid);
```

---

## 8. 局部变量

- 小写 + 匈牙利前缀（MFC）：
  - `int nRet`、`std::string strValue`、`size_t nLen`、`bool bOk`

---

## 9. 常量

- 类/文件内常量：Google `k` 前缀 + PascalCase（MFC 仅对资源 ID 规定全大写，C++ 常量采用 Google 补充）：
  - `kInvalidTimerId`、`kInitialSize`、`kMaxPacketSize`、`kInvalidSubscriptionId`
- 枚举值：Google `k` 前缀 + PascalCase：
  - `kTrace`、`kStarted`、`kNeedMore`、`kCmdPing`

---

## 10. 宏与接口标识

- 宏：全大写 + 下划线（MFC / Google 一致）：
  - `MAX_PACKET_SIZE`、`ASIO_STANDALONE`
- 接口标识函数：`IID_` 前缀 + 接口名（COM 体系）：
  - `IID_INetwork()`、`IID_ILogger()`、`IID_IModule()`

---

## 11. 回调 / 类型别名

- `using` 别名：PascalCase + 语义后缀（本项目约定）：
  - `AcceptCallback`、`DataCallback`、`CloseCallback`、`MessageHandler`、`EventHandler`

---

## 12. 与现有代码的差异（迁移说明）

| 项 | 本文档（新代码） | 现有代码 |
| --- | --- | --- |
| 类命名 | `C` 前缀（`CMyApplication`） | 无前缀（`MyApplication`） |
| 成员变量 | `m_` 前缀（`m_strName`） | 尾下划线（`name_`） |
| 接口 | `I` 前缀（`IUnknown`） | `I` 前缀（一致） |
| 方法 | PascalCase | PascalCase（一致） |
| 常量 | `k` 前缀 | `k` 前缀（一致） |
| 参数 | 匈牙利前缀（`nPort`） | 小写无前缀（`port`） |

- 历史代码（Common / ServerCore / Demo / ServerA）保持现状，不在本轮统一重命名；
- **新代码**（新项目、新组件、新模块）遵循本文档；
- 如需迁移历史代码，应在独立提交中进行，且不影响接口行为。

---

## 13. 检查要点

新增或修改代码时，对照检查：

1. 类/接口/枚举类型命名是否符合 §4（`C` / `I` 前缀 + PascalCase）？
2. 成员变量是否使用 `m_` 前缀 + 匈牙利前缀（§5）？
3. 方法与参数命名是否符合 §6 / §7？
4. 常量、宏、接口标识是否符合 §9 / §10？
5. 新组件/模块/服务是否使用了统一的 `Component` / `Module` / `Service` 后缀（§4.4）？
