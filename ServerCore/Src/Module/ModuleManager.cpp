#include "Module/ModuleManager.h"

namespace sc {

/// @brief 创建模块管理器。
ModuleManager::ModuleManager()
{
}

/// @brief 销毁模块管理器，释放所有已注册模块。
ModuleManager::~ModuleManager()
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
bool ModuleManager::RegisterModule(IModule* module)
{
    if (module == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const char* name = module->GetName();
    if (name == nullptr || name[0] == '\0')
    {
        return false;
    }
    if (indexByName_.find(name) != indexByName_.end())
    {
        return false; // 名称重复
    }
    module->AddRef();
    indexByName_[name] = modules_.size();
    modules_.push_back(Entry(module));
    return true;
}

/// @brief 根据名称获取模块。
///
/// @param name 模块名称。
///
/// @return 借用指针（不增加引用计数）；未找到返回 nullptr。
IModule* ModuleManager::GetModule(const char* name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, size_t>::const_iterator it = indexByName_.find(name);
    if (it == indexByName_.end())
    {
        return nullptr;
    }
    return modules_[it->second].module;
}

/// @brief 查询模块当前状态。
///
/// @param name 模块名称。
///
/// @return 模块状态；未找到返回 kCreated。
ModuleState ModuleManager::GetModuleState(const char* name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, size_t>::const_iterator it = indexByName_.find(name);
    if (it == indexByName_.end())
    {
        return ModuleState::kCreated;
    }
    return modules_[it->second].state;
}

/// @brief 反注册模块。
///
/// 移除模块并释放引用（锁外释放，避免模块析构时重入）。
///
/// @param name 模块名称。
///
/// @return true 移除成功；false 未找到。
bool ModuleManager::UnregisterModule(const char* name)
{
    IModule* module = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, size_t>::iterator it = indexByName_.find(name);
        if (it == indexByName_.end())
        {
            return false;
        }
        size_t index = it->second;
        module = modules_[index].module;
        modules_.erase(modules_.begin() + static_cast<std::ptrdiff_t>(index));
        // 向量删除后重建名称索引
        indexByName_.clear();
        for (size_t i = 0; i < modules_.size(); ++i)
        {
            indexByName_[modules_[i].module->GetName()] = i;
        }
    }
    module->Release();
    return true;
}

/// @brief 移除并释放所有模块。
void ModuleManager::Clear()
{
    std::vector<IModule*> toRelease;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        toRelease.reserve(modules_.size());
        for (size_t i = 0; i < modules_.size(); ++i)
        {
            toRelease.push_back(modules_[i].module);
        }
        modules_.clear();
        indexByName_.clear();
    }
    for (size_t i = 0; i < toRelease.size(); ++i)
    {
        toRelease[i]->Release();
    }
}

/// @brief 已注册模块数量。
size_t ModuleManager::Size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return modules_.size();
}

/// @brief 统一初始化所有模块。
///
/// 按注册顺序调用 Initialize；任一步失败时逆序关闭已初始化的模块。
///
/// @return true 全部初始化成功；false 存在失败并已回滚。
bool ModuleManager::InitializeAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < modules_.size(); ++i)
    {
        Entry& e = modules_[i];
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
bool ModuleManager::StartAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < modules_.size(); ++i)
    {
        Entry& e = modules_[i];
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
void ModuleManager::StopAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = modules_.size(); i > 0; --i)
    {
        Entry& e = modules_[i - 1];
        if (e.state == ModuleState::kStarted)
        {
            e.module->Stop();
            e.state = ModuleState::kStopped;
        }
    }
}

/// @brief 统一关闭所有模块（逆序）。
void ModuleManager::ShutdownAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = modules_.size(); i > 0; --i)
    {
        Entry& e = modules_[i - 1];
        if (e.state != ModuleState::kCreated && e.state != ModuleState::kShutdown)
        {
            e.module->Shutdown();
            e.state = ModuleState::kShutdown;
        }
    }
}

/// @brief 逆序关闭所有已初始化的模块（回滚辅助）。
void ModuleManager::RollbackInitialized()
{
    for (size_t i = modules_.size(); i > 0; --i)
    {
        Entry& e = modules_[i - 1];
        if (e.state == ModuleState::kInitialized)
        {
            e.module->Shutdown();
            e.state = ModuleState::kShutdown;
        }
    }
}

/// @brief 逆序停止并关闭已启动的模块（回滚辅助）。
void ModuleManager::RollbackStarted()
{
    // 先停止已启动的模块
    for (size_t i = modules_.size(); i > 0; --i)
    {
        Entry& e = modules_[i - 1];
        if (e.state == ModuleState::kStarted)
        {
            e.module->Stop();
            e.state = ModuleState::kStopped;
        }
    }
    // 再关闭已初始化的模块
    for (size_t i = modules_.size(); i > 0; --i)
    {
        Entry& e = modules_[i - 1];
        if (e.state == ModuleState::kInitialized || e.state == ModuleState::kStopped)
        {
            e.module->Shutdown();
            e.state = ModuleState::kShutdown;
        }
    }
}

} // namespace sc
