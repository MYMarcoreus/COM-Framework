#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <string>

#include "Component/IUnknown.h"

namespace sc {

/// @brief 组件管理器。
///
/// 负责注册、获取、移除和清空组件，并持有每个已注册组件的一个引用。
/// 注册成功后，组件所有权归管理器；移除或清空时释放引用。
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

private:
    struct Entry
    {
        IUnknown* component;
    };

    std::map<std::string, Entry> m_mapComponents;
    mutable std::mutex m_mutex;
};

} // namespace sc
