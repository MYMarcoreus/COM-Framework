#pragma once

#include "Module/InterfaceId.h"

namespace sc {

/// @brief 接口表条目（由接口映射宏生成，查表驱动 QueryInterface）。
///
/// 每一条目对应模块实现的一个接口：接口标识 + 指针转换函数。
/// 查询时按接口标识逐条匹配，命中即调用转换函数取出对应接口指针。
struct InterfaceEntry
{
    const InterfaceId* piid;      // 接口标识常量地址（IID_XXX() 返回的静态对象）
    void* (*pfnGet)(void* pThis); // 指针转换函数：从实例指针取出对应接口指针（借用）
};

/// @brief 接口指针转换函数。
///
/// 由 SC_INTERFACE_ENTRY 宏实例化；多继承下用 static_cast 自动调整 this，
/// 无需手工计算接口偏移（区别于 ATL 的非标准 offsetofclass）。
///
/// @tparam Derived 具体模块类
/// @tparam Iface   接口类型
template <typename Derived, typename Iface>
void* InterfaceGetter(void* pThis)
{
    return static_cast<Iface*>(static_cast<Derived*>(pThis));
}

} // namespace sc

/// @brief 在模块类内声明接口映射表（放在类体任意位置，通常 protected 区）。
///
/// 声明 GetInterfaceTable() 与 QueryInterfaceImpl() 覆写；
/// 定义在 .cpp 中由 SC_BEGIN_INTERFACE_MAP / SC_END_INTERFACE_MAP 生成。
#define SC_DECLARE_INTERFACE_MAP()                                      \
protected:                                                              \
    static const sc::InterfaceEntry* GetInterfaceTable();               \
    void* QueryInterfaceImpl(const sc::InterfaceId& iid) override;

/// @brief 在 .cpp 中开始定义接口映射表（与 SC_END_INTERFACE_MAP 成对）。
///
/// @param ClassName 当前模块类名
/// @param BaseClass 基类名（通常 sc::CModule，作为查询兜底）
#define SC_BEGIN_INTERFACE_MAP(ClassName, BaseClass)                    \
    const sc::InterfaceEntry* ClassName::GetInterfaceTable()            \
    {                                                                   \
        using SC_IfcClass = ClassName;                                  \
        static const sc::InterfaceEntry s_table[] = {

/// @brief 接口表条目：声明本类实现的一个接口（自动推导接口标识）。
///
/// 只应在 SC_BEGIN_INTERFACE_MAP 与 SC_END_INTERFACE_MAP 之间使用。
/// 通过 InterfaceIdOf<IfaceType> 特化自动推导接口标识（需先声明特化）；
/// 无特化的自定义接口请用 SC_INTERFACE_ENTRY_EX 显式指定。
///
/// @param IfaceType 接口类型（I 前缀，如 ILogger）
#define SC_INTERFACE_ENTRY(IfaceType)                                  \
        { &sc::InterfaceIdOf<IfaceType>::Get(),                        \
          &sc::InterfaceGetter<SC_IfcClass, IfaceType> },

/// @brief 接口表条目：声明本类实现的一个接口（显式指定接口标识）。
///
/// 供无 InterfaceIdOf 特化的自定义接口使用；内置接口优先用 SC_INTERFACE_ENTRY。
///
/// @param IfaceType 接口类型（I 前缀，如 ILogger）
/// @param IidExpr   接口标识表达式（如 IID_ILogger()）
#define SC_INTERFACE_ENTRY_EX(IfaceType, IidExpr)                      \
        { &IidExpr, &sc::InterfaceGetter<SC_IfcClass, IfaceType> },

/// @brief 结束接口映射表并生成 QueryInterfaceImpl 定义。
///
/// 生成的 QueryInterfaceImpl 逐条查表，未命中时回落到 BaseClass 的
/// QueryInterfaceImpl（例如 CModule 暴露 IModule）。
///
/// @param ClassName 当前模块类名（须与 SC_BEGIN_INTERFACE_MAP 一致）
/// @param BaseClass 基类名（须与 SC_BEGIN_INTERFACE_MAP 一致）
#define SC_END_INTERFACE_MAP(ClassName, BaseClass)                      \
        { nullptr, nullptr }                                            \
        };                                                              \
        return s_table;                                                 \
    }                                                                   \
                                                                        \
    void* ClassName::QueryInterfaceImpl(const sc::InterfaceId& iid)     \
    {                                                                   \
        const sc::InterfaceEntry* pEntry = ClassName::GetInterfaceTable(); \
        for (; pEntry->piid != nullptr; ++pEntry)                       \
        {                                                               \
            if (iid == *pEntry->piid)                                   \
            {                                                           \
                return pEntry->pfnGet(this);                            \
            }                                                           \
        }                                                               \
        return BaseClass::QueryInterfaceImpl(iid);                      \
    }

// ====================================================================
// 便捷封装：SC_DEFINE_INTERFACE_MAP（一步生成接口映射表定义）
// 把"SC_BEGIN_INTERFACE_MAP + SC_INTERFACE_ENTRY* + SC_END_INTERFACE_MAP"
// 三部曲合并为单宏，接口数量由可变参数自动展开（最多 8 个）。
// ====================================================================

/// @brief 内部辅助：展开 1 个接口条目。
#define SC_IMAP_ENTRIES_1(I0) SC_INTERFACE_ENTRY(I0)

/// @brief 内部辅助：展开 2 个接口条目。
#define SC_IMAP_ENTRIES_2(I0, I1) SC_INTERFACE_ENTRY(I0) SC_INTERFACE_ENTRY(I1)

/// @brief 内部辅助：展开 3 个接口条目。
#define SC_IMAP_ENTRIES_3(I0, I1, I2) SC_INTERFACE_ENTRY(I0) SC_INTERFACE_ENTRY(I1) SC_INTERFACE_ENTRY(I2)

/// @brief 内部辅助：展开 4 个接口条目。
#define SC_IMAP_ENTRIES_4(I0, I1, I2, I3) SC_INTERFACE_ENTRY(I0) SC_INTERFACE_ENTRY(I1) SC_INTERFACE_ENTRY(I2) SC_INTERFACE_ENTRY(I3)

/// @brief 内部辅助：展开 5 个接口条目。
#define SC_IMAP_ENTRIES_5(I0, I1, I2, I3, I4) SC_INTERFACE_ENTRY(I0) SC_INTERFACE_ENTRY(I1) SC_INTERFACE_ENTRY(I2) SC_INTERFACE_ENTRY(I3) SC_INTERFACE_ENTRY(I4)

/// @brief 内部辅助：展开 6 个接口条目。
#define SC_IMAP_ENTRIES_6(I0, I1, I2, I3, I4, I5) SC_INTERFACE_ENTRY(I0) SC_INTERFACE_ENTRY(I1) SC_INTERFACE_ENTRY(I2) SC_INTERFACE_ENTRY(I3) SC_INTERFACE_ENTRY(I4) SC_INTERFACE_ENTRY(I5)

/// @brief 内部辅助：展开 7 个接口条目。
#define SC_IMAP_ENTRIES_7(I0, I1, I2, I3, I4, I5, I6) SC_INTERFACE_ENTRY(I0) SC_INTERFACE_ENTRY(I1) SC_INTERFACE_ENTRY(I2) SC_INTERFACE_ENTRY(I3) SC_INTERFACE_ENTRY(I4) SC_INTERFACE_ENTRY(I5) SC_INTERFACE_ENTRY(I6)

/// @brief 内部辅助：展开 8 个接口条目。
#define SC_IMAP_ENTRIES_8(I0, I1, I2, I3, I4, I5, I6, I7) SC_INTERFACE_ENTRY(I0) SC_INTERFACE_ENTRY(I1) SC_INTERFACE_ENTRY(I2) SC_INTERFACE_ENTRY(I3) SC_INTERFACE_ENTRY(I4) SC_INTERFACE_ENTRY(I5) SC_INTERFACE_ENTRY(I6) SC_INTERFACE_ENTRY(I7)

/// @brief 内部辅助：统计可变参数个数（0~8）。
#define SC_IMAP_NARG(...)  SC_IMAP_NARG_(__VA_ARGS__, SC_IMAP_RSEQ())
#define SC_IMAP_NARG_(...) SC_IMAP_ARGN(__VA_ARGS__)
#define SC_IMAP_ARGN(_1, _2, _3, _4, _5, _6, _7, _8, N, ...) N
#define SC_IMAP_RSEQ()     8, 7, 6, 5, 4, 3, 2, 1, 0

/// @brief 内部辅助：按接口个数选择并展开对应数量的接口条目。
#define SC_IMAP_SELECT(...)   SC_IMAP_CALL(SC_IMAP_NARG(__VA_ARGS__), __VA_ARGS__)
#define SC_IMAP_CALL(N, ...)  SC_IMAP_CALL_(N, __VA_ARGS__)
#define SC_IMAP_CALL_(N, ...) SC_IMAP_ENTRIES_##N(__VA_ARGS__)

/// @brief 在 .cpp 中一步定义接口映射表（等价于 BEGIN + ENTRIES + END 三部曲）。
///
/// 生成 GetInterfaceTable() 与 QueryInterfaceImpl() 的定义；
/// 未命中时回落到 BaseClass::QueryInterfaceImpl。
///
/// 用法：
/// @code
///   // 单接口
///   SC_DEFINE_INTERFACE_MAP(CExampleService,
///       sc::CModule, sc::INetworkHandler)
///   // 多接口（最多 8 个）
///   SC_DEFINE_INTERFACE_MAP(CService, sc::CModule, sc::INetworkHandler, sc::IMetrics)
/// @endcode
///
/// 需要显式接口标识的条目（无 InterfaceIdOf 特化的接口）请改用
/// SC_BEGIN_INTERFACE_MAP / SC_INTERFACE_ENTRY_EX / SC_END_INTERFACE_MAP 三部曲。
///
/// @param ClassName 当前模块类名
/// @param BaseClass 基类名（查询兜底，通常 sc::CModule）
/// @param ...       本类实现的接口类型列表（自动推导接口标识，最多 8 个）
#define SC_DEFINE_INTERFACE_MAP(ClassName, BaseClass, ...)           \
    SC_BEGIN_INTERFACE_MAP(ClassName, BaseClass)                        \
        SC_IMAP_SELECT(__VA_ARGS__)                                     \
    SC_END_INTERFACE_MAP(ClassName, BaseClass)
