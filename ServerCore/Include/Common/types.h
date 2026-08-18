#pragma once

#include "Network/network_types.h"

namespace sc {

/// @brief 网络连接标识（来自 Common 基础库）。
using ConnectionId = common::ConnectionId;

/// @brief 无效连接标识。
static const ConnectionId kInvalidConnectionId = common::kInvalidConnectionId;

} // namespace sc
