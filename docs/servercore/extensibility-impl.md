# 扩展机制 — 实现文档

> 配套使用文档：[extensibility-usage.md](extensibility-usage.md)
> 源码：`ServerCore/Module/InterfaceDecl.h`、`InterfaceMap.h`、`RefObject.*`

框架的「可扩展性」由三个机制共同支撑：**接口自动声明**、**接口查询链**、**组合根装配**。

## 1. 接口自动声明（SC_INTERFACE 宏）

`SC_INTERFACE(ClassName, Name, Guid)` 一键生成四件事（详见 [module-system-impl.md](module-system-impl.md)）：

1. 前向声明 `class ClassName`；
2. `IID_##ClassName()`（GUID 常量）；
3. `InterfaceIdOf<ClassName>` 特化（类型 ↔ 接口标识绑定，供 `Resolve<T>()`）；
4. `class ClassName : public virtual IUnknown`。

> 新接口只需一行宏，即获得：iid 常量、类型绑定、IUnknown 基类——三者一致，杜绝「iid 与类型不匹配」。

## 2. 接口查询链（QueryInterfaceImpl）

- `CRefObject::QueryInterface`：`IID_IUnknown` → 基类视图；其余转虚函数 `QueryInterfaceImpl(iid)`；
- 子类覆写 `QueryInterfaceImpl`：先查自己的接口，未命中调 `CModule::QueryInterfaceImpl(iid)`（向上回溯）；
- 因此一个对象可实现**多个接口**，通过 `Self<T>()` / `WeakSelf<T>()`（RTTI）取任意视图（见 module-system-impl）。

## 3. 组合根装配（RegisterModules）

- `CMyApplication::RegisterModules()` 是**唯一装配点**：默认装配（IConfig/ILogger/IMetrics）→ 基础设施 →
  业务模块，全部 `RegisterModule(iid, new CModule())`；
- 拓扑排序（`ComputeStartOrder`）按 `AddDependency` 声明决定初始化/启动顺序（见 module-system-impl）；
- 新增模块 = 新增一行 `RegisterModule`，不改框架核心——满足「不修改 ServerCore 核心代码即可扩展」。

## 4. 扩展点汇总

| 扩展类型 | 入口 | 机制 |
|---|---|---|
| 新接口 | `SC_INTERFACE` | 自动 iid + 类型绑定 + IUnknown |
| 新模块 | 继承 `CModule` + 注册 | `RegisterModule` + 拓扑排序 |
| 新协议 | `MakeMessageExtractor()` | 消息流水线提取器（[messaging-usage.md](messaging-usage.md)） |
| 新服务器 | 继承 `CMyApplication` | `RegisterModules()` 组合根 |
