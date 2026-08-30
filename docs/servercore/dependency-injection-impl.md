# 依赖注入 — 实现文档

> 配套使用文档：[dependency-injection-usage.md](dependency-injection-usage.md)
> 源码：`ServerCore/Module/ResolveContext.h`、`InterfaceDecl.h`、`ModuleManager.*`

## 1. CResolveContext

```cpp
class CResolveContext
{
private:
    CModuleManager& m_manager;   // 引用成员（非指针）：由 InitializeAll 注入，恒有效
};
```

- 构造：`InitializeAll()` 内 `CResolveContext context(*this)` 传给每个模块的 `Initialize(ctx)`；
- 只保存对 `CModuleManager` 的引用，不做任何缓存——每次 `Resolve` 都实时查管理器注册表。

## 2. Resolve 两条路径

```cpp
// 无参版：编译期「类型 ↔ 接口标识」绑定
template <typename T> T* CResolveContext::Resolve() const
{
    return m_manager.Resolve<T>(InterfaceIdOf<T>::Get());
}

// 显式版：供无特化的自定义接口
template <typename T> T* CResolveContext::Resolve(const InterfaceId& iid) const
{
    return m_manager.Resolve<T>(iid);
}
```

- `InterfaceIdOf<T>` 特化由 `SC_INTERFACE` 宏自动生成（`static const InterfaceId& Get()`）；
- 返回**借用指针不增计数**——解析不是持有，调用方用 `ScopedInterfacePtr` 接管才增计数；
  因此可安全用于单测注入 mock（mock 模块注册到临时 manager 即可）。

## 3. CModuleManager::Resolve<T>(iid)

```cpp
T* Resolve<T>(const InterfaceId& iid)
{
    IModule* pModule = GetModuleByIid(iid);     // 查 m_mapIndexByIid
    if (pModule == nullptr) return nullptr;
    IUnknown* pUnk = pModule->QueryInterface(iid);  // COM 接口查询（借用）
    return static_cast<T*>(pUnk);
}
```

- `GetModuleByIid` 用 `m_mapIndexByIid`（multimap）定位；同接口多实例用 `GetModulesByIid`（`equal_range`）。

## 4. 生命周期保证的实现

- **硬依赖**：`AddDependency(iid)` → `CModuleManager::ComputeStartOrder()` 拓扑排序（Kahn，依赖在前）；
- **可选依赖**：不 `AddDependency`，靠 `RegisterModule` 的注册顺序（初始化也按注册序）保证先注册者先就绪；
- `AddDependency` 在模块构造函数中调用，无效 iid 忽略；`GetDependencies()` 返回该列表供排序。
