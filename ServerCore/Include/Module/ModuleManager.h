#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Module/IModule.h"

namespace sc {

/// @brief 模块管理器（COM 思想：注册即持有引用，生命周期统一编排）。
///
/// 与 CComponentManager 一致，注册时对模块 AddRef，移除/清空时 Release。
/// 生命周期统一管理：
///   - 初始化 / 启动：按注册顺序（先注册先启动，依赖在前）
///   - 停止 / 关闭：逆序（后启动先停止，栈式释放）
/// 某一步失败时逆序回滚已完成的模块，保证状态一致。
class CModuleManager
{
public:
    CModuleManager();

    ~CModuleManager();

    // 注册模块：验证名称非空且不重复，成功后持有模块一个引用。
    bool RegisterModule(IModule* module);

    // 根据名称获取模块（借用指针，不增加引用计数）。
    IModule* GetModule(const char* name) const;

    // 查询模块当前状态。
    ModuleState GetModuleState(const char* name) const;

    // 反注册模块：释放引用并移除。
    bool UnregisterModule(const char* name);

    // 移除并释放所有模块。
    void Clear();

    // 已注册模块数量。
    size_t Size() const;

    // 统一初始化所有模块（按注册顺序）。失败时逆序关闭已初始化的模块。
    bool InitializeAll();

    // 统一启动所有模块（按注册顺序）。失败时逆序停止并关闭已启动的模块。
    bool StartAll();

    // 统一停止所有模块（逆序）。
    void StopAll();

    // 统一关闭所有模块（逆序）。
    void ShutdownAll();

private:
    struct Entry
    {
        IModule* module;
        ModuleState state;

        explicit Entry(IModule* m) : module(m), state(ModuleState::kCreated) {}
    };

    // 逆序关闭所有处于已初始化状态的模块（回滚辅助）。
    void RollbackInitialized();

    // 逆序停止已启动的模块，再逆序关闭已初始化的模块（回滚辅助）。
    void RollbackStarted();

    std::vector<Entry> m_vecModules;
    std::map<std::string, size_t> m_mapIndexByName;
    mutable std::mutex m_mutex;
};

} // namespace sc
