#include "Module/ModuleManager.h"

namespace sc {

/// @brief 创建模块管理器。
CModuleManager::CModuleManager()
{
}

/// @brief 销毁模块管理器，释放所有已注册模块。
CModuleManager::~CModuleManager()
{
    Clear();
}

/// @brief 注册模块。
///
/// 校验模块非空、名称非空且不重复；成功后持有模块一个引用。
///
/// @param module 模块接口指针。
///
/// @return true 注册成功；false 参数非法或名称重复。
bool CModuleManager::RegisterModule(IModule* pModule)
{
    if (pModule == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* strName = pModule->GetName();
    if (strName == nullptr || strName[0] == '\0')
    {
        return false;
    }
    if (m_mapIndexByName.find(strName) != m_mapIndexByName.end())
    {
        return false; // 名称重复
    }
    pModule->AddRef();
    m_mapIndexByName[strName] = m_vecModules.size();
    m_vecModules.push_back(Entry(pModule));
    return true;
}

/// @brief 根据名称获取模块。
///
/// @param strName 模块名称。
///
/// @return 借用指针（不增加引用计数）；未找到返回 nullptr。
IModule* CModuleManager::GetModule(const char* strName) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<std::string, size_t>::const_iterator it = m_mapIndexByName.find(strName);
    if (it == m_mapIndexByName.end())
    {
        return nullptr;
    }
    return m_vecModules[it->second].module;
}

/// @brief 查询模块当前状态。
///
/// @param strName 模块名称。
///
/// @return 模块状态；未找到返回 kCreated。
ModuleState CModuleManager::GetModuleState(const char* strName) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<std::string, size_t>::const_iterator it = m_mapIndexByName.find(strName);
    if (it == m_mapIndexByName.end())
    {
        return ModuleState::kCreated;
    }
    return m_vecModules[it->second].state;
}

/// @brief 反注册模块。
///
/// 移除模块并释放引用（锁外释放，避免模块析构时重入）。
///
/// @param strName 模块名称。
///
/// @return true 移除成功；false 未找到。
bool CModuleManager::UnregisterModule(const char* strName)
{
    IModule* pModule = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<std::string, size_t>::iterator it = m_mapIndexByName.find(strName);
        if (it == m_mapIndexByName.end())
        {
            return false;
        }
        size_t nIndex = it->second;
        pModule = m_vecModules[nIndex].module;
        m_vecModules.erase(m_vecModules.begin() + static_cast<std::ptrdiff_t>(nIndex));
        // 向量删除后重建名称索引
        m_mapIndexByName.clear();
        for (size_t i = 0; i < m_vecModules.size(); ++i)
        {
            m_mapIndexByName[m_vecModules[i].module->GetName()] = i;
        }
    }
    pModule->Release();
    return true;
}

/// @brief 移除并释放所有模块。
void CModuleManager::Clear()
{
    std::vector<IModule*> vecToRelease;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        vecToRelease.reserve(m_vecModules.size());
        for (size_t i = 0; i < m_vecModules.size(); ++i)
        {
            vecToRelease.push_back(m_vecModules[i].module);
        }
        m_vecModules.clear();
        m_mapIndexByName.clear();
    }
    for (size_t i = 0; i < vecToRelease.size(); ++i)
    {
        vecToRelease[i]->Release();
    }
}

/// @brief 已注册模块数量。
size_t CModuleManager::Size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_vecModules.size();
}

/// @brief 统一初始化所有模块。
///
/// 按注册顺序调用 Initialize；任一步失败时逆序关闭已初始化的模块。
///
/// @return true 全部初始化成功；false 存在失败并已回滚。
bool CModuleManager::InitializeAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = 0; i < m_vecModules.size(); ++i)
    {
        Entry& e = m_vecModules[i];
        if (e.state != ModuleState::kCreated)
        {
            continue; // 幂等：跳过非初始状态
        }
        if (!e.module->Initialize())
        {
            RollbackInitialized();
            return false;
        }
        e.state = ModuleState::kInitialized;
    }
    return true;
}

/// @brief 统一启动所有模块。
///
/// 按注册顺序启动已初始化的模块；任一步失败时逆序停止并关闭已启动的模块。
///
/// @return true 全部启动成功；false 存在失败并已回滚。
bool CModuleManager::StartAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = 0; i < m_vecModules.size(); ++i)
    {
        Entry& e = m_vecModules[i];
        if (e.state != ModuleState::kInitialized)
        {
            continue; // 只启动已初始化的模块
        }
        if (!e.module->Start())
        {
            RollbackStarted();
            return false;
        }
        e.state = ModuleState::kStarted;
    }
    return true;
}

/// @brief 统一停止所有模块（逆序）。
void CModuleManager::StopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = m_vecModules.size(); i > 0; --i)
    {
        Entry& e = m_vecModules[i - 1];
        if (e.state == ModuleState::kStarted)
        {
            e.module->Stop();
            e.state = ModuleState::kStopped;
        }
    }
}

/// @brief 统一关闭所有模块（逆序）。
void CModuleManager::ShutdownAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = m_vecModules.size(); i > 0; --i)
    {
        Entry& e = m_vecModules[i - 1];
        if (e.state != ModuleState::kCreated && e.state != ModuleState::kShutdown)
        {
            e.module->Shutdown();
            e.state = ModuleState::kShutdown;
        }
    }
}

/// @brief 逆序关闭所有已初始化的模块（回滚辅助）。
void CModuleManager::RollbackInitialized()
{
    for (size_t i = m_vecModules.size(); i > 0; --i)
    {
        Entry& e = m_vecModules[i - 1];
        if (e.state == ModuleState::kInitialized)
        {
            e.module->Shutdown();
            e.state = ModuleState::kShutdown;
        }
    }
}

/// @brief 逆序停止并关闭已启动的模块（回滚辅助）。
void CModuleManager::RollbackStarted()
{
    // 先停止已启动的模块
    for (size_t i = m_vecModules.size(); i > 0; --i)
    {
        Entry& e = m_vecModules[i - 1];
        if (e.state == ModuleState::kStarted)
        {
            e.module->Stop();
            e.state = ModuleState::kStopped;
        }
    }
    // 再关闭已初始化的模块
    for (size_t i = m_vecModules.size(); i > 0; --i)
    {
        Entry& e = m_vecModules[i - 1];
        if (e.state == ModuleState::kInitialized || e.state == ModuleState::kStopped)
        {
            e.module->Shutdown();
            e.state = ModuleState::kShutdown;
        }
    }
}

} // namespace sc
