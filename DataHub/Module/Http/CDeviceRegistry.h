#pragma once

#include <map>
#include <mutex>
#include <string>

namespace datahub {

/// @brief 设备注册表（账号凭据，修复"裸信 X-Client-Id"）。
///
/// 每个设备（浏览器/CLI）首次 `POST /api/device/register` 注册 clientId，
/// 服务端签发一个随机**设备令牌（secret）**；此后身份相关请求须带
/// `X-Client-Id` + `X-Token`，本表校验令牌归属，杜绝伪造他人账号。
/// 令牌为"持物凭据（bearer）"：局域网无 TLS 下仍有被嗅探风险（文档注明），
/// 但已消灭"自报 id 即他人"的裸信任。纯内存（服务重启清空，客户端会自动重注册）。
class CDeviceRegistry
{
   public:
    CDeviceRegistry();

    // 注册（幂等）：已注册返回既有令牌，未注册生成新令牌。
    std::string Register(const std::string& strClientId);

    // 校验令牌归属（常数时间比较）；@return true 合法。
    bool Verify(const std::string& strClientId, const std::string& strToken) const;

    // 当前已注册设备数。
    std::size_t Count() const;

    // 清空（进程内注册表；重启即清，客户端自动重注册）。
    void Clear();

   private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::string> m_mapToken;  // clientId → token
};

}  // namespace datahub
