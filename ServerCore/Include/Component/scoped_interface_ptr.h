#pragma once

#include <cstddef>
#include <type_traits>

#include "Component/i_unknown.h"

namespace sc {

/// @brief 组件接口智能指针。
///
/// 构造时增加引用计数，析构时释放引用计数（RAII）。
/// 类似 CComPtr，但面向本项目 C++11/Linux 组件模型。
///
/// @tparam T 接口类型，必须是 IUnknown 派生类型。
template <typename T>
class ScopedInterfacePtr
{
    static_assert(std::is_base_of<IUnknown, T>::value,
                  "ScopedInterfacePtr<T> 要求 T 必须是 IUnknown 派生接口");

public:
    ScopedInterfacePtr() : ptr_(nullptr) {}

    explicit ScopedInterfacePtr(T* ptr) : ptr_(ptr)
    {
        if (ptr_ != nullptr)
        {
            ptr_->AddRef();
        }
    }

    ScopedInterfacePtr(const ScopedInterfacePtr& other) : ptr_(other.ptr_)
    {
        if (ptr_ != nullptr)
        {
            ptr_->AddRef();
        }
    }

    ScopedInterfacePtr(ScopedInterfacePtr&& other) noexcept : ptr_(other.ptr_)
    {
        other.ptr_ = nullptr;
    }

    ~ScopedInterfacePtr()
    {
        if (ptr_ != nullptr)
        {
            ptr_->Release();
        }
    }

    // 拷贝赋值。
    ScopedInterfacePtr& operator=(const ScopedInterfacePtr& other)
    {
        if (this != &other)
        {
            Reset(other.ptr_);
        }
        return *this;
    }

    // 移动赋值。
    ScopedInterfacePtr& operator=(ScopedInterfacePtr&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
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
        if (ptr_ != nullptr)
        {
            ptr_->Release();
        }
        ptr_ = ptr;
    }

    // 返回裸指针（借用，不转移所有权）。
    T* Get() const { return ptr_; }

    // 指针访问运算符。
    T* operator->() const { return ptr_; }

    // 解引用运算符。
    T& operator*() const { return *ptr_; }

    // 判断是否持有有效指针。
    explicit operator bool() const { return ptr_ != nullptr; }

    // 判断是否为空。
    bool operator!() const { return ptr_ == nullptr; }

    // 与另一个智能指针比较。
    bool operator==(const ScopedInterfacePtr& other) const { return ptr_ == other.ptr_; }

    // 与另一个智能指针比较。
    bool operator!=(const ScopedInterfacePtr& other) const { return ptr_ != other.ptr_; }

    // 与裸指针比较。
    bool operator==(T* ptr) const { return ptr_ == ptr; }

    // 与裸指针比较。
    bool operator!=(T* ptr) const { return ptr_ != ptr; }

    // 与 nullptr 比较。
    bool operator==(std::nullptr_t) const { return ptr_ == nullptr; }

    // 与 nullptr 比较。
    bool operator!=(std::nullptr_t) const { return ptr_ != nullptr; }

private:
    T* ptr_;
};

} // namespace sc
