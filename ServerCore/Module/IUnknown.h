#pragma once

#include "Module/InterfaceId.h"

namespace sc {

/// @brief 所有模块接口的基类。
///
/// 借鉴 COM 模块模型思想：接口查询 + 引用计数 + 生命周期管理。
/// 本实现面向 Linux/C++11，不依赖 Windows COM。
///
/// @note 接口查询返回借用的指针，不增加引用计数。
class IUnknown
{
public:
    // 查询指定接口。
    // 返回借用的接口指针（不增加引用计数）；未找到返回 nullptr。
    virtual void* QueryInterface(const InterfaceId& iid) = 0;

    // 增加引用计数。
    virtual unsigned int AddRef() = 0;

    // 减少引用计数，归零时销毁模块。
    virtual unsigned int Release() = 0;

protected:
    virtual ~IUnknown() {}
};

} // namespace sc
