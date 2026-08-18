#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Component/Component.h"
#include "Component/IUnknown.h"

namespace sc {

/// @brief 组件管理器。
///
/// 负责注册、获取、移除和清空组件，并持有每个已注册组件的一个引用。
/// 注册成功后，组件所有权归管理器；移除或清空时释放引用。
/// 提供统一的生命周期编排（InitializeAll / StartAll / StopAll / ShutdownAll）。
class CComponentManager
{
public:
    CComponentManager();

    ~CComponentManager();

    // 注册组件。注册成功后管理器持有组件的一个引用。
    bool RegisterComponent(const InterfaceId& iid, IUnknown* pComponent);

    // 根据接口标识获取组件，返回借用指针，不增加引用计数。
    IUnknown* GetComponent(const InterfaceId& iid) const;

    // 移除并释放指定接口标识对应的组件。
    bool RemoveComponent(const InterfaceId& iid);

    // 移除并释放所有组件。
    void Clear();

    // 返回已注册组件数量。
    size_t Size() const;

    // 统一初始化所有组件（按注册顺序）；失败返回 false。
    bool InitializeAll();

    // 统一启动所有组件；失败返回 false。
    bool StartAll();

    // 统一停止所有组件。
    void StopAll();

    // 统一关闭所有组件。
    void ShutdownAll();

private:
    struct Entry
    {
        IUnknown* component;
    };

    // 收集所有 CComponent 派生组件（锁外调用）。
    void CollectComponents(std::vector<CComponent*>& vecOut) const;

    std::map<std::string, Entry> m_mapComponents;
    mutable std::mutex m_mutex;
};

} // namespace sc
