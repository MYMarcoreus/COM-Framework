# 模块系统 — 实现文档

> 配套使用文档：[module-system-usage.md](module-system-usage.md)
> 源码：`ServerCore/Module/*`

## 1. InterfaceId（128 位 GUID）

- 存储：`uint64_t m_nHigh / m_nLow` + `std::string m_strName`；全零即无效（`IsValid()`）。
- `ParseGuid`：跳过 `-`，每 2 个 hex 字符合成 1 字节，满 16 字节后前 8→`m_nHigh`、后 8→`m_nLow`；非法置全零。
- `operator<`：先高后低比较，可作 `std::map` 键。
- `IID_XXX()`：内联函数返回**函数内 static 常量**（进程内单实例地址）。
- `InterfaceIdOf<T>`：主模板仅前向声明；特化由 `SC_INTERFACE` 自动生成 `static const InterfaceId& Get()`。

### SC_INTERFACE 宏展开

`SC_INTERFACE(ClassName, Name, Guid)` 生成：
1. 前向声明 `class ClassName`；
2. `IID_##ClassName()` 静态常量；
3. `InterfaceIdOf<ClassName>` 特化（类型 ↔ 接口标识绑定）；
4. `class ClassName : public virtual IUnknown`（`SC_INTERFACE_BASE` 可追加虚拟基类）。

## 2. ScopedInterfacePtr<T>（RAII）

- `static_assert T` 派生自 `IUnknown`；构造（裸指针/拷贝）→ `AddRef`；移动 → 窃取置空；析构 → `Release`；
- `Reset(T*)`：**先对新指针 `AddRef` 再释放旧指针**（防同对象自赋值提前销毁）；
- `Adopt(T*)`：包装**已持有引用**的指针不额外 AddRef（内部用，配合 `CWeakPtr::Lock`）；
- `operator->/* /explicit operator bool`；接口查询返回借用指针不增计数。

## 3. CWeakPtr<T> / CLifetime（弱引用安全）

```cpp
class CLifetime { std::atomic<bool> m_bAlive; std::mutex m_mutex; };
```

- `CWeakPtr` 持 `T* m_ptr` + `std::weak_ptr<CLifetime> m_pLifetime`；
- Lifetime 由 `CRefObject` 构造时 `make_shared`；其析构**或** Release 归零均持 `m_mutex` 后 `MarkDead`；
- `Lock()`：`weak_ptr.lock()` 升级 shared（保证 Lifetime 对象未释放）→ 锁 `m_mutex` → `IsAlive()` 假则返空，
  真则 `m_ptr->AddRef()` 后 `Adopt` 成强引用——「存活检查 + AddRef」与「归零 MarkDead + 析构」**同一把锁内互斥**，
  绝不访问已销毁内存；
- `Expired()`：`m_ptr==nullptr || m_pLifetime.expired()`。

## 4. CRefObject（引用计数基础）

- 成员：`std::atomic<unsigned int> m_nRefCount`（初始 1）、`std::shared_ptr<CLifetime> m_pLifetime`；
- `AddRef`：`fetch_add(1)+1` 无锁原子；`Release`：`fetch_sub(1)-1`，归零时持锁 `MarkDead` + `delete this`；
- `QueryInterface`：无效 iid → null；`IID_IUnknown` → `static_cast<IUnknown*>(this)`；其余转 `QueryInterfaceImpl`（默认 null，子类覆写）；
- `Self<T=IUnknown>()`：`dynamic_cast<T*>(this)`（RTTI 视图）+ 强引用；`WeakSelf<T>()`：同 RTTI + `m_pLifetime` 构造弱引用；
  `detail::IsSelfable<T>` 约束 T 为 IUnknown 派生且非指针/引用/cv。

## 5. CModule

- 继承 `CRefObject` + `IModule`；成员：`std::atomic<ModuleState> m_state`（初始 `kCreated`）、`m_strName`、`m_vecDependencies`；
- `GetName / GetState / GetDependencies` 均 **final**（框架管理）；生命周期四方法**纯虚**；
- `AddDependency(iid)` protected（构造中调用），供拓扑排序；`SetState` private，仅 `friend CModuleManager` 调用。

## 6. CModuleManager

**数据成员**：`std::vector<Entry> m_vecModules`（`Entry{IModule* module; InterfaceId iid;}`，iid 无效=按名注册）、
`std::map<std::string,size_t> m_mapIndexByName`、`std::multimap<InterfaceId,size_t> m_mapIndexByIid`（同接口多实例）、
`mutable std::recursive_mutex m_mutex`。

- **RegisterModule（接管型）**：不额外 AddRef，直接接管创建者引用；空名/重名/iid 无效时自行 `Release` 并返回 false；
- **InitializeAll**：持锁 → 构造 `CResolveContext(*this)` → `ComputeStartOrder()` → 按序 `Initialize`（跳过非 kCreated，
  幂等），任一步失败 `RollbackInitialized()`（逆序 Shutdown 已初始化者）并返 false；**StartAll** 同构，失败
  `RollbackStarted()`（先逆序 Stop 再逆序 Shutdown）；
- **StopAll / ShutdownAll**：逆序遍历，仅处理对应状态；
- **拓扑排序 ComputeStartOrder**：由注册 iid 建 `mapProvider`（iid → 首个提供者索引），据 `GetDependencies()` 建
  入度/后继图，Kahn 稳定排序（deque FIFO 保持同层注册序）；环或依赖缺失的模块**追加到末尾**；
- **Resolve<T>(iid)**：`GetModuleByIid` → `QueryInterface(iid)` → `static_cast<T*>`（借用）；`GetModulesByIid` 用
  `equal_range` 返回全部实例；`Snapshot()` 输出 `{name, iid, state, status}`；
- **锁外 Release**：Unregister / Clear 均在锁外 `Release`（避免析构重入死锁），Unregister 删除后重建索引；
- **超时版**：`Stop/ShutdownAllWithTimeout` 用 `steady_clock` 逐次检查。

**线程模型**：所有 `*All` 生命周期在 `m_mutex` 内**串行**执行（recursive_mutex 支持回调内重入查询）；
`Register / GetModule / Snapshot` 等查询持锁；`AddRef/Release` 无锁原子可任意线程；
`WeakSelf().Lock()` 是**跨线程安全**升级（可安全用于异步回调线程）。

## 7. CResolveContext

- 仅持 `CModuleManager& m_manager`（引用成员，由 `InitializeAll` 注入）；
- `Resolve<T>()` 无参版：`m_manager.Resolve<T>(InterfaceIdOf<T>::Get())`（编译期「类型 ↔ iid」绑定）；
- `Resolve<T>(iid)` 显式版：供无 `InterfaceIdOf` 特化的自定义接口；
- 返回借用指针不增计数（利于单测注入 mock）。
