#pragma once

#include <cstddef>
#include <type_traits>

#include "Module/IUnknown.h"

namespace sc {

/// @brief 模块接口智能指针。
///
/// 构造时增加引用计数，析构时释放引用计数（RAII）。
/// 类似 CComPtr，但面向本项目 C++11/Linux 模块模型。
///
/// @tparam T 接口类型，必须是 IUnknown 派生类型。
template <typename T>
class ScopedInterfacePtr
{
    static_assert(std::is_base_of<IUnknown, T>::value,
                  "ScopedInterfacePtr<T> 要求 T 必须是 IUnknown 派生接口");

public:
    ScopedInterfacePtr() : m_ptr(nullptr) {}

    explicit ScopedInterfacePtr(T* ptr) : m_ptr(ptr)
    {
        if (m_ptr != nullptr)
        {
            m_ptr->AddRef();
        }
    }

    ScopedInterfacePtr(const ScopedInterfacePtr& other) : m_ptr(other.m_ptr)
    {
        if (m_ptr != nullptr)
        {
            m_ptr->AddRef();
        }
    }

    ScopedInterfacePtr(ScopedInterfacePtr&& other) noexcept : m_ptr(other.m_ptr)
    {
        other.m_ptr = nullptr;
    }

    ~ScopedInterfacePtr()
    {
        if (m_ptr != nullptr)
        {
            m_ptr->Release();
        }
    }

    // 拷贝赋值。
    ScopedInterfacePtr& operator=(const ScopedInterfacePtr& other)
    {
        if (this != &other)
        {
            Reset(other.m_ptr);
        }
        return *this;
    }

    // 移动赋值。
    ScopedInterfacePtr& operator=(ScopedInterfacePtr&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    // 从裸指针赋值。
    ScopedInterfacePtr& operator=(T* ptr)
    {
        Reset(ptr);
        return *this;
    }

    // 释放当前引用并接管新指针（可为空）。
    void Reset(T* ptr = nullptr)
    {
        if (ptr != nullptr)
        {
            ptr->AddRef();
        }
        if (m_ptr != nullptr)
        {
            m_ptr->Release();
        }
        m_ptr = ptr;
    }

    // 返回裸指针（借用，不转移所有权）。
    T* Get() const { return m_ptr; }

    // 接管已持有的引用（不额外 AddRef）。
    // 仅内部使用：调用方必须保证 ptr 已具有有效引用（如 CWeakPtr::Lock 已 AddRef）。
    static ScopedInterfacePtr<T> Adopt(T* ptr)
    {
        ScopedInterfacePtr<T> sp;
        sp.m_ptr = ptr;
        return sp;
    }

    // 指针访问运算符。
    T* operator->() const { return m_ptr; }

    // 解引用运算符。
    T& operator*() const { return *m_ptr; }

    // 判断是否持有有效指针。
    explicit operator bool() const { return m_ptr != nullptr; }

    // 判断是否为空。
    bool operator!() const { return m_ptr == nullptr; }

    // 与另一个智能指针比较。
    bool operator==(const ScopedInterfacePtr& other) const { return m_ptr == other.m_ptr; }

    // 与另一个智能指针比较。
    bool operator!=(const ScopedInterfacePtr& other) const { return m_ptr != other.m_ptr; }

    // 与裸指针比较。
    bool operator==(T* ptr) const { return m_ptr == ptr; }

    // 与裸指针比较。
    bool operator!=(T* ptr) const { return m_ptr != ptr; }

    // 与 nullptr 比较。
    bool operator==(std::nullptr_t) const { return m_ptr == nullptr; }

    // 与 nullptr 比较。
    bool operator!=(std::nullptr_t) const { return m_ptr != nullptr; }

private:
    T* m_ptr;
};

} // namespace sc
