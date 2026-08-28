#pragma once

#include <cstdint>

namespace common {
namespace network {

/// @brief 网络连接标识。
using ConnectionId = std::uint64_t;

/// @brief 无效连接标识。
static const ConnectionId kInvalidConnectionId = 0;

} // namespace network
} // namespace common
