#pragma once

#include "Module/InterfaceId.h"

namespace sc {

// 接口声明宏展开时会用到 InterfaceIdOf 主模板（见 InterfaceId.h）。
// 本头仅提供接口声明宏，不定义任何类型。

} // namespace sc

/// @brief 声明一个 COM 风格接口（自动生成 IID_XXX() 与 InterfaceIdOf 特化）。
///
/// 在接口头文件中使用，接口体紧随宏书写：
/// @code
///   SC_INTERFACE(ILogger, "sc::ILogger", "7f70d36c-e774-49c0-9f0e-0d59b5c0adf8")
///   {
///   public:
///       virtual void Info(const std::string& strMessage) = 0;
///   };
/// @endcode
/// 展开后自动生成：接口类（继承 IUnknown）、IID_##ClassName()、InterfaceIdOf 特化，
/// 新增接口无需再手动维护 InterfaceIdOf 特化。
///
/// @param ClassName 接口类型名（I 前缀，如 ILogger）
/// @param Name      可读名（如 "sc::ILogger"，用于日志 / 快照）
/// @param Guid      标准 GUID 字符串（8-4-4-4-12，用 uuidgen 生成）
#define SC_INTERFACE(ClassName, Name, Guid)                             \
    class ClassName;                                                    \
    inline const InterfaceId& IID_##ClassName()                         \
    {                                                                   \
        static const InterfaceId iid(Name, Guid);                       \
        return iid;                                                     \
    }                                                                   \
    template <> struct InterfaceIdOf<ClassName>                         \
    {                                                                   \
        static const InterfaceId& Get() { return IID_##ClassName(); }   \
    };                                                                  \
    class ClassName : public virtual IUnknown

/// @brief 声明一个继承既有接口的 COM 风格接口（自动生成 IID 与 InterfaceIdOf 特化）。
///
/// @param BaseInterface 被继承的接口类型（如 IUnknown 或另一接口）
#define SC_INTERFACE_BASE(ClassName, Name, Guid, BaseInterface)         \
    class ClassName;                                                    \
    inline const InterfaceId& IID_##ClassName()                         \
    {                                                                   \
        static const InterfaceId iid(Name, Guid);                       \
        return iid;                                                     \
    }                                                                   \
    template <> struct InterfaceIdOf<ClassName>                         \
    {                                                                   \
        static const InterfaceId& Get() { return IID_##ClassName(); }   \
    };                                                                  \
    class ClassName : public virtual BaseInterface
