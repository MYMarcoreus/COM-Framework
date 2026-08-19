#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Module/IModule.h"

namespace sc {

/// @brief 模块快照（供健康检查 / 日志 / 管理接口使用）。
struct ModuleSnapshot
{
    std::string strName;   // 模块名称（可为空）
    std::string strIid;    // 接口标识注册键（可为空）
    ModuleState state;     // 当前生命周期状态
    std::string strStatus; // 状态描述（GetStatus）
};

/// @brief 模块管理器（COM 思想：注册即持有引用，生命周期统一编排）。
///
/// 统一管理器：既可按名字注册业务模块，也可按接口标识注册服务模块（服务定位）。
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
    bool RegisterModule(IModule* pModule);

    // 按接口标识注册模块（服务定位；模块可有可无名称），成功后持有模块一个引用。
    bool RegisterModule(const InterfaceId& iid, IModule* pModule);

    // 接管型注册（按名字）：接管调用方 new 出来的模块（不额外 AddRef）。
    // 成功时管理器持有该引用；失败时管理器负责 Release 并返回 false，调用方无需释放。
    bool RegisterModuleOwned(IModule* pModule);

    // 接管型注册（按接口标识）：语义同上。
    bool RegisterModuleOwned(const InterfaceId& iid, IModule* pModule);

    // 根据名称获取模块（借用指针，不增加引用计数）。
    IModule* GetModule(const char* strName) const;

    // 根据接口标识获取模块（借用指针，不增加引用计数）。
    IModule* GetModuleByIid(const InterfaceId& iid) const;

    // 查询模块当前状态。
    ModuleState GetModuleState(const char* strName) const;

    // 模块是否存在（按名称）。
    bool HasModule(const char* strName) const;

    // 模块是否存在（按接口标识）。
    bool HasModuleByIid(const InterfaceId& iid) const;

    // 生成所有模块的结构化快照（供健康检查 / 日志 / 管理接口）。
    std::vector<ModuleSnapshot> Snapshot() const;

    // 反注册模块：释放引用并移除。
    bool UnregisterModule(const char* strName);

    // 反注册接口标识对应的模块：释放引用并移除。
    bool UnregisterModuleByIid(const InterfaceId& iid);

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

    // 生成所有模块的状态报告（每行一个模块）。
    std::string StatusReport() const;

    // 带超时地统一停止所有模块；返回 true 表示在超时前完成。
    bool StopAllWithTimeout(uint32_t nTimeoutMs);

    // 带超时地统一关闭所有模块；返回 true 表示在超时前完成。
    bool ShutdownAllWithTimeout(uint32_t nTimeoutMs);

private:
    // 设置模块生命周期状态（friend 访问 CModule::SetState）。
    static void SetModuleState(IModule* pModule, ModuleState state);

    // 收集所有已注册模块（锁外调用）。
    void CollectModules(std::vector<IModule*>& vecOut) const;
    struct Entry
    {
        IModule* module;
        std::string strIid; // 接口标识注册键（空表示按名字注册）

        explicit Entry(IModule* m, const std::string& iid = std::string())
            : module(m), strIid(iid) {}
    };

    // 逆序关闭所有处于已初始化状态的模块（回滚辅助）。
    void RollbackInitialized();

    // 逆序停止已启动的模块，再逆序关闭已初始化的模块（回滚辅助）。
    void RollbackStarted();

    std::vector<Entry> m_vecModules;
    std::map<std::string, size_t> m_mapIndexByName;
    std::map<std::string, size_t> m_mapIndexByIid;
    // 递归互斥量：生命周期编排在持锁状态下调用模块回调，
    // 回调内再查询管理器（GetModule/GetModuleByIid）时允许重入，避免死锁。
    mutable std::recursive_mutex m_mutex;
};

} // namespace sc
