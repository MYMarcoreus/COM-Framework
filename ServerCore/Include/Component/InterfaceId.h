#pragma once

namespace sc {

/// @brief 接口唯一标识。
///
/// 使用进程内唯一的字符串常量地址标识一个接口。
/// 每个接口通过独立的内联函数返回其标识，保证地址稳定唯一。
using InterfaceId = const char*;

/// @brief 获取 IUnknown 接口标识。
inline const InterfaceId& IID_IUnknown()
{
    static const InterfaceId iid = "sc::IUnknown";
    return iid;
}

} // namespace sc
