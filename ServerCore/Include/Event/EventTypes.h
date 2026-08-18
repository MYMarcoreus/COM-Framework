#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace sc {

/// @brief 无效订阅标识。
static const std::uint64_t kInvalidSubscriptionId = 0;

/// @brief 订阅标识，用于取消订阅。
using SubscriptionId = std::uint64_t;

/// @brief 事件类型标识（字符串，语义化，进程内唯一）。
using EventType = std::string;

/// @brief 事件数据。
///
/// data / size 为借用指针，仅在本次事件分发期间有效，处理器不应长期持有。
struct Event
{
    EventType type;   // 事件类型
    const void* data; // 事件负载（借用指针）
    size_t size;      // 负载字节数

    Event() : type(), data(nullptr), size(0) {}

    Event(const EventType& eventType, const void* payload, size_t payloadSize)
        : type(eventType), data(payload), size(payloadSize)
    {
    }
};

/// @brief 事件处理器。
using EventHandler = std::function<void(const Event& event)>;

} // namespace sc
