// ============================================================
// 文件：TokenBucket.cpp
// 模块：sserver::transport
// 功能：令牌桶实现：首次使用时装满满桶，此后按时间差补充、封顶突发额度。
// 知识点：浮点累加的时间驱动补充、夹紧突发上限、线性等待估算
// ============================================================

#include "sserver/transport/udp/TokenBucket.h"

#include <algorithm>
#include <cmath>

namespace sserver {
namespace transport {

TokenBucket::TokenBucket(double rateBytesPerSecond, double burstBytes)
        : m_rateBytesPerSecond(rateBytesPerSecond > 0.0 ? rateBytesPerSecond : 0.0),
          m_burstBytes(burstBytes > 0.0 ? burstBytes : 0.0),
          m_tokens(m_burstBytes) {
}

void TokenBucket::refill(std::uint64_t nowNs) const {
    if (!m_primed) {
        // 第一次用到时钟时才建立基准点：构造时刻未知，从 0 起算会把两个
        // 时间戳的差算成一个天文数字，直接把桶灌满。
        m_lastRefillNs = nowNs;
        m_primed = true;
        return;
    }
    if (nowNs <= m_lastRefillNs) {
        // 时间倒退（调用方传了旧时间戳）不补充也不倒扣，只更新基准点。
        m_lastRefillNs = nowNs;
        return;
    }
    const double elapsedSeconds =
            static_cast<double>(nowNs - m_lastRefillNs) / 1000000000.0;
    m_lastRefillNs = nowNs;
    m_tokens = std::min(m_burstBytes, m_tokens + elapsedSeconds * m_rateBytesPerSecond);
}

bool TokenBucket::tryAcquire(std::size_t bytes, std::uint64_t nowNs, std::uint64_t *waitNs) {
    refill(nowNs);
    const double needed = static_cast<double>(bytes);
    if (m_tokens >= needed) {
        m_tokens -= needed;
        if (waitNs != nullptr) {
            *waitNs = 0;
        }
        return true;
    }
    if (waitNs != nullptr) {
        const double shortfall = needed - m_tokens;
        if (m_rateBytesPerSecond <= 0.0) {
            // 速率为 0 意味着永远补不上：给出一个"无限等待"的哨兵值，
            // 让调用方据此放弃而不是真的去睡。
            *waitNs = UINT64_MAX;
        } else {
            *waitNs = static_cast<std::uint64_t>(
                    std::ceil(shortfall / m_rateBytesPerSecond * 1000000000.0));
        }
    }
    return false;
}

double TokenBucket::availableBytes(std::uint64_t nowNs) const {
    refill(nowNs);
    return m_tokens;
}

}  // namespace transport
}  // namespace sserver
