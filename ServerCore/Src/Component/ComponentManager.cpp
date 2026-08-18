#include "Component/ComponentManager.h"

#include <vector>

namespace sc {

/// @brief 创建组件管理器。
CComponentManager::CComponentManager()
{
}

/// @brief 销毁组件管理器。
CComponentManager::~CComponentManager()
{
    Clear();
}

/// @brief 注册组件。
///
/// 以接口标识为键，同一接口标识只能注册一次。
/// 注册成功后管理器持有组件的一个引用（AddRef）。
///
/// @param iid 接口标识。
/// @param component 组件对象。
///
/// @return true 注册成功；false 参数无效或接口已存在。
bool CComponentManager::RegisterComponent(const InterfaceId& iid, IUnknown* component)
{
    if (iid == nullptr || component == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string key(iid);
    if (m_mapComponents.find(key) != m_mapComponents.end())
    {
        return false; // 已存在同标识组件
    }
    component->AddRef(); // 管理器持有引用
    Entry entry;
    entry.component = component;
    m_mapComponents[key] = entry;
    return true;
}

/// @brief 获取组件。
///
/// @param iid 接口标识。
///
/// @return 借用指针，不增加引用计数；未找到返回 nullptr。
IUnknown* CComponentManager::GetComponent(const InterfaceId& iid) const
{
    if (iid == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<std::string, Entry>::const_iterator it = m_mapComponents.find(std::string(iid));
    if (it == m_mapComponents.end())
    {
        return nullptr;
    }
    return it->second.component;
}

/// @brief 移除并释放指定组件。
///
/// @param iid 接口标识。
///
/// @return true 移除成功；false 未找到。
bool CComponentManager::RemoveComponent(const InterfaceId& iid)
{
    if (iid == nullptr)
    {
        return false;
    }
    IUnknown* component = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<std::string, Entry>::iterator it = m_mapComponents.find(std::string(iid));
        if (it == m_mapComponents.end())
        {
            return false;
        }
        component = it->second.component;
        m_mapComponents.erase(it);
    }
    if (component != nullptr)
    {
        component->Release(); // 释放管理器持有的引用
    }
    return true;
}

/// @brief 清空所有组件并释放引用。
void CComponentManager::Clear()
{
    std::vector<IUnknown*> toRelease;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::map<std::string, Entry>::iterator it = m_mapComponents.begin(); it != m_mapComponents.end(); ++it)
        {
            toRelease.push_back(it->second.component);
        }
        m_mapComponents.clear();
    }
    for (size_t i = 0; i < toRelease.size(); ++i)
    {
        toRelease[i]->Release();
    }
}

/// @brief 返回已注册组件数量。
size_t CComponentManager::Size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mapComponents.size();
}

} // namespace sc
