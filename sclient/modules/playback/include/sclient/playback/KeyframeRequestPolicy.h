// ============================================================
// 文件：KeyframeRequestPolicy.h
// 模块：sclient::playback
// 功能：断档后请求关键帧的冷却策略：RTT 未知时用固定冷却，已知时按
//       2×RTT+200ms 自适应并夹在上下限之间。
// 知识点：冷却窗口防刷屏、RTT 驱动的自适应、纯逻辑可单测
// ============================================================

#ifndef SCLIENT_PLAYBACK_KEYFRAME_REQUEST_POLICY_H
#define SCLIENT_PLAYBACK_KEYFRAME_REQUEST_POLICY_H

#include <cstdint>

namespace sclient {
namespace playback {

// 为什么需要冷却：一次丢包风暴里几十帧会同时"断档"，逐个请求会把控制
// 通道刷爆，而且服务端按 500ms 冷却才真正出一个 IDR——多发无益。
class KeyframeRequestPolicy {
public:
    struct Limits {
        std::uint64_t m_minCooldownNs = 300ULL * 1000ULL * 1000ULL;
        std::uint64_t m_maxCooldownNs = 1500ULL * 1000ULL * 1000ULL;
        // RTT 未知时直接取上限：链路状况不明时保守一点。
        std::uint64_t m_unknownRttCooldownNs = 1500ULL * 1000ULL * 1000ULL;
    };

    KeyframeRequestPolicy() = default;
    explicit KeyframeRequestPolicy(const Limits &limits) : m_limits(limits) {}

    // 0 表示未知。
    void setRttNs(std::uint64_t rttNs) { m_rttNs = rttNs; }

    // 冷却时长：未知 RTT 用固定值；已知时 2×RTT + 200ms，夹在上下限之间。
    std::uint64_t cooldownNs() const;

    bool shouldRequest(std::uint64_t nowNs) const;

    // 记录一次实际发出的请求。首次请求不受冷却限制。
    void markRequested(std::uint64_t nowNs);

    std::uint64_t requestCount() const { return m_requestCount; }

private:
    Limits m_limits;
    std::uint64_t m_rttNs = 0;
    std::uint64_t m_lastRequestNs = 0;
    bool m_hasRequested = false;
    std::uint64_t m_requestCount = 0;
};

}  // namespace playback
}  // namespace sclient

#endif  // SCLIENT_PLAYBACK_KEYFRAME_REQUEST_POLICY_H
