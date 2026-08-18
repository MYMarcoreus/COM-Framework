#include "Component/ComponentManager.h"

#include <chrono>
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
bool CComponentManager::RegisterComponent(const InterfaceId& iid, IUnknown* pComponent)
{
    if (iid == nullptr || pComponent == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string strKey(iid);
    if (m_mapComponents.find(strKey) != m_mapComponents.end())
    {
        return false; // 已存在同标识组件
    }
    pComponent->AddRef(); // 管理器持有引用
    Entry entry;
    entry.component = pComponent;
    m_mapComponents[strKey] = entry;
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
    IUnknown* pComponent = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<std::string, Entry>::iterator it = m_mapComponents.find(std::string(iid));
        if (it == m_mapComponents.end())
        {
            return false;
        }
        pComponent = it->second.component;
        m_mapComponents.erase(it);
    }
    if (pComponent != nullptr)
    {
        pComponent->Release(); // 释放管理器持有的引用
    }
    return true;
}

/// @brief 清空所有组件并释放引用。
void CComponentManager::Clear()
{
    std::vector<IUnknown*> vecToRelease;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::map<std::string, Entry>::iterator it = m_mapComponents.begin(); it != m_mapComponents.end(); ++it)
        {
            vecToRelease.push_back(it->second.component);
        }
        m_mapComponents.clear();
    }
    for (size_t i = 0; i < vecToRelease.size(); ++i)
    {
        vecToRelease[i]->Release();
    }
}

/// @brief 返回已注册组件数量。
size_t CComponentManager::Size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mapComponents.size();
}

/// @brief 统一初始化所有组件。
///
/// 按注册顺序调用 Initialize；任一步失败返回 false。
bool CComponentManager::InitializeAll()
{
    std::vector<CComponent*> vecComponents;
    CollectComponents(vecComponents);
    for (size_t i = 0; i < vecComponents.size(); ++i)
    {
        if (!vecComponents[i]->Initialize())
        {
            return false;
        }
    }
    return true;
}

/// @brief 统一启动所有组件。
///
/// 按注册顺序调用 Start；任一步失败返回 false。
bool CComponentManager::StartAll()
{
    std::vector<CComponent*> vecComponents;
    CollectComponents(vecComponents);
    for (size_t i = 0; i < vecComponents.size(); ++i)
    {
        if (!vecComponents[i]->Start())
        {
            return false;
        }
    }
    return true;
}

/// @brief 统一停止所有组件（逆序）。
void CComponentManager::StopAll()
{
    std::vector<CComponent*> vecComponents;
    CollectComponents(vecComponents);
    for (size_t i = vecComponents.size(); i > 0; --i)
    {
        vecComponents[i - 1]->Stop();
    }
}

/// @brief 统一关闭所有组件（逆序）。
void CComponentManager::ShutdownAll()
{
    std::vector<CComponent*> vecComponents;
    CollectComponents(vecComponents);
    for (size_t i = vecComponents.size(); i > 0; --i)
    {
        vecComponents[i - 1]->Shutdown();
    }
}

/// @brief 生成所有组件的状态报告。
///
/// @return 多行状态文本，每行对应一个组件。
std::string CComponentManager::StatusReport() const
{
    std::vector<CComponent*> vecComponents;
    CollectComponents(vecComponents);
    std::string strReport;
    for (size_t i = 0; i < vecComponents.size(); ++i)
    {
        if (!strReport.empty())
        {
            strReport += "\n";
        }
        strReport += vecComponents[i]->GetStatus();
    }
    return strReport;
}

/// @brief 带超时地统一停止所有组件。
///
/// @param nTimeoutMs 总超时毫秒数。
///
/// @return true 表示在超时前完成；false 表示已超时（跳过剩余组件）。
bool CComponentManager::StopAllWithTimeout(uint32_t nTimeoutMs)
{
    std::vector<CComponent*> vecComponents;
    CollectComponents(vecComponents);
    std::chrono::steady_clock::time_point tpBegin = std::chrono::steady_clock::now();
    bool bComplete = true;
    for (size_t i = vecComponents.size(); i > 0; --i)
    {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tpBegin).count() >= static_cast<int64_t>(nTimeoutMs))
        {
            bComplete = false;
            break;
        }
        vecComponents[i - 1]->Stop();
    }
    return bComplete;
}

/// @brief 带超时地统一关闭所有组件。
///
/// @param nTimeoutMs 总超时毫秒数。
///
/// @return true 表示在超时前完成；false 表示已超时（跳过剩余组件）。
bool CComponentManager::ShutdownAllWithTimeout(uint32_t nTimeoutMs)
{
    std::vector<CComponent*> vecComponents;
    CollectComponents(vecComponents);
    std::chrono::steady_clock::time_point tpBegin = std::chrono::steady_clock::now();
    bool bComplete = true;
    for (size_t i = vecComponents.size(); i > 0; --i)
    {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tpBegin).count() >= static_cast<int64_t>(nTimeoutMs))
        {
            bComplete = false;
            break;
        }
        vecComponents[i - 1]->Shutdown();
    }
    return bComplete;
}

/// @brief 收集所有 CComponent 派生组件。
///
/// @param vecOut 输出组件列表（按注册顺序）。
void CComponentManager::CollectComponents(std::vector<CComponent*>& vecOut) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::map<std::string, Entry>::const_iterator it = m_mapComponents.begin();
         it != m_mapComponents.end(); ++it)
    {
        CComponent* pComponent = dynamic_cast<CComponent*>(it->second.component);
        if (pComponent != nullptr)
        {
            vecOut.push_back(pComponent);
        }
    }
}

} // namespace sc
