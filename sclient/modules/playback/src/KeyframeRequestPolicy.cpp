// ============================================================
// 文件：KeyframeRequestPolicy.cpp
// 模块：sclient::playback
// 功能：冷却计算与请求判定的实现。
// 知识点：上下限夹紧、首次请求放行、单调时钟时间戳比较
// ============================================================

#include "sclient/playback/KeyframeRequestPolicy.h"

#include <algorithm>

namespace sclient {
namespace playback {

namespace {
constexpr std::uint64_t kRttSafetyNs = 200ULL * 1000ULL * 1000ULL;
}

std::uint64_t KeyframeRequestPolicy::cooldownNs() const {
    if (m_rttNs == 0) {
        return m_limits.m_unknownRttCooldownNs;
    }
    // 两倍 RTT 覆盖"请求发出去 + 关键帧回来"的一个来回，再加 200ms 余量
    // 给服务端的编码与打包。夹在上下限之间：太小会刷屏，太大会让恢复很慢。
    const std::uint64_t target = 2ULL * m_rttNs + kRttSafetyNs;
    return std::max(m_limits.m_minCooldownNs,
                    std::min(m_limits.m_maxCooldownNs, target));
}

bool KeyframeRequestPolicy::shouldRequest(std::uint64_t nowNs) const {
    if (!m_hasRequested) {
        return true;
    }
    return nowNs > m_lastRequestNs && nowNs - m_lastRequestNs >= cooldownNs();
}

void KeyframeRequestPolicy::markRequested(std::uint64_t nowNs) {
    m_hasRequested = true;
    m_lastRequestNs = nowNs;
    ++m_requestCount;
}

}  // namespace playback
}  // namespace sclient
