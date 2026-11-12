// ============================================================
// 文件：KeyframeRecoverySignal.h
// 模块：sserver::base
// 功能：丢帧后的关键帧补偿请求：任意线程置位，编码线程每帧取走一次；
//       内置冷却时间，防止多个会话同时请求造成 IDR 风暴。
// 知识点：原子标志的取走即清、CAS 保证只授予一次、冷却窗口
// ============================================================

#ifndef SSERVER_BASE_KEYFRAME_RECOVERY_SIGNAL_H
#define SSERVER_BASE_KEYFRAME_RECOVERY_SIGNAL_H

#include <atomic>
#include <cstdint>

namespace sserver {
namespace base {

// 为什么需要冷却：关键帧比普通帧大一个数量级，连续几个 IDR 会把令牌桶
// 的突发额度一口气抽干，网络排队反而更糟。500 毫秒对应 30fps 下的 15 帧，
// 足够让一次补帧真正生效，又不至于让恢复慢到用户察觉。
class KeyframeRecoverySignal {
public:
    static constexpr std::uint64_t kNeverGranted = ~0ULL;

    explicit KeyframeRecoverySignal(std::uint64_t cooldownNs = 500ULL * 1000ULL * 1000ULL)
            : m_cooldownNs(cooldownNs) {
    }

    // 请求一次关键帧。重复请求只是把同一个标志再置一次，不会排队。
    void request() {
        m_pending.store(true, std::memory_order_release);
    }

    bool pending() const {
        return m_pending.load(std::memory_order_acquire);
    }

    // 编码线程每帧调用一次。
    // 冷却期内的请求**保留**而不是丢弃：等到期后仍会授予一次，否则
    // 恰好落在冷却窗口里的那次丢帧就永远得不到补偿。
    // @return 本次应当强制输出关键帧时返回 true（标志被取走）
    bool consume(std::uint64_t nowNs) {
        if (!m_pending.load(std::memory_order_acquire)) {
            return false;
        }
        const std::uint64_t lastGrant = m_lastGrantNs.load(std::memory_order_acquire);
        // 用最大值当"从未授予"的哨兵：不能用 0，因为 monotonicNowNs 在
        // 进程刚启动时可能确实是 0，那会把首次授予误判成处于冷却期。
        if (lastGrant != kNeverGranted && nowNs - lastGrant < m_cooldownNs) {
            return false;
        }
        // CAS 而不是 store：两个线程同时 consume 时只允许一个拿到授权。
        bool expected = true;
        if (!m_pending.compare_exchange_strong(expected, false,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
            return false;
        }
        m_lastGrantNs.store(nowNs, std::memory_order_release);
        return true;
    }

    std::uint64_t cooldownNs() const { return m_cooldownNs; }

private:
    std::uint64_t m_cooldownNs;
    std::atomic<bool> m_pending{false};
    std::atomic<std::uint64_t> m_lastGrantNs{kNeverGranted};
};

}  // namespace base
}  // namespace sserver

#endif  // SSERVER_BASE_KEYFRAME_RECOVERY_SIGNAL_H
