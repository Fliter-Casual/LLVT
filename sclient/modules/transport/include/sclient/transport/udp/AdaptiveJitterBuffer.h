// ============================================================
// 文件：AdaptiveJitterBuffer.h
// 模块：sclient::transport
// 功能：自适应抖动缓冲策略（头文件内联实现）。按 P95 抖动、分片丢包率与
//       跳帧率把网络质量分级，在 Bypass / LowLatency / Smooth 三模式间
//       迟滞切换，并给出目标延迟与等待/深度上限，供 UDP 收流侧调用。
// 知识点：自适应抖动缓冲、P95 百分位、迟滞状态机、constexpr 调参常量、
//         非对称调参（恶化慢恢复快）、std::clamp
// ============================================================

#ifndef SCLIENT_TRANSPORT_ADAPTIVE_JITTER_BUFFER_H
#define SCLIENT_TRANSPORT_ADAPTIVE_JITTER_BUFFER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "sclient/base/LatencyStats.h"
#include "sclient/transport/udp/JitterBufferTypes.h"

namespace sclient {
namespace transport {

// ==================== 质量分级阈值 ====================

// 用 P95 而不是均值：抖动分布是长尾的，均值会把偶发尖峰摊平；而用户感知到
// 的卡顿恰恰来自尾部。P95 表示「95% 的帧都不会比这更抖」。
constexpr double kQualityPoorP95Ms = 50.0;      //  P95 最low抖动的阈值
constexpr double kQualityPoorLossPercent = 1.0; // 丢包率的阈值(poor)
constexpr double kQualityPoorSkipPercent = 5.0; // 跳帧率的阈值(poor)
constexpr double kQualityFairP95Ms = 10.0;    //  P95 中等抖动的阈值(fair)
constexpr double kQualityFairLossPercent = 0.1; // 丢包率的阈值(fair)
constexpr double kQualityFairSkipPercent = 1.0; // 跳帧率的阈值(fair)
constexpr double kQualityGoodP95Ms = 1.0;     //  P95 最佳抖动的阈值(good)
constexpr double kQualityGoodLossPercent = 0.01; // 丢包率的阈值(good)

// ==================== 模式切换阈值 ====================

// 所有迁移都要求「连续多次」或「持续一段时间」，这是迟滞：网络在阈值附近
// 来回抖时，模式不能跟着抖，否则每次切换都伴随目标延迟的跳变。
//
// 非对称是关键：升档门槛低（10 次），降档门槛高（20 次 / 持续 3 秒）。
// 多留一点缓冲的代价是几十毫秒延迟，少留的代价是画面撕裂或停帧。
constexpr double kBypassToLowLatencyJitterMs = 1.0; //  Bypass 到 LowLatency 的抖动阈值
constexpr int kBypassToLowLatencySamples = 10;    //  Bypass 到 LowLatency 的样本数阈值
constexpr double kLowLatencyToBypassJitterMs = 0.5;
constexpr int kLowLatencyToBypassSamples = 20;
constexpr double kLowLatencyToSmoothP95Ms = 10.0; //  LowLatency 到 Smooth 的 P95 抖动阈值
constexpr double kSmoothToLowLatencyP95Ms = 5.0;   //  Smooth 到 LowLatency 的 P95 抖动阈值
constexpr std::uint64_t kSmoothDowngradeHoldNs = 3ULL * 1000ULL * 1000ULL * 1000ULL; //  Smooth 到 LowLatency 的降级保持时间阈值

// 窗口样本少于这个数时 P95 没有统计意义，先只攒数据、不做任何切换。
constexpr std::size_t kAutoModeMinSamples = 10;

// 抖动统计窗口：最近 100 个样本，每 4 个样本刷新一次快照。
constexpr std::size_t kJitterWindowSamples = 100;
constexpr std::size_t kJitterSnapshotRefreshInterval = 4;

// ==================== 统计辅助 ====================

// 跳帧率的分母是「已出帧 + 已跳帧」而不是「已出帧」：否则跳过 3 帧只发出
// 1 帧时会算出 300%，这个数再喂回质量评级就完全失真了。
inline double jitterBufferSkipRatePercent(std::uint64_t emittedFrames,
                                          std::uint64_t skippedFrames) {
    const long double kOutcomes = static_cast<long double>(emittedFrames)
            + static_cast<long double>(skippedFrames);
    if (kOutcomes == 0.0L) {
        return 0.0;
    }
    return static_cast<double>(static_cast<long double>(skippedFrames) * 100.0L / kOutcomes);
}

// ==================== 配置 ====================

// 成员自带默认值，调用方只需覆盖关心的字段。
struct AdaptiveJitterBufferConfig {
    int m_minDelayMs = 2;        /**< 目标延迟下限：网络再好也留一点吸收突发 */
    int m_maxDelayMs = 120;      /**< 目标延迟硬上限：抖动再大也不无限等下去 */
    double m_safetyFactor = 1.5;  /**< 低延迟模式安全系数 */
    double m_smoothFactor = 2.5;  /**< 流畅模式安全系数 */
    int m_baseMaxWaitMs = 80;     /**< 最长等待基准，按质量档位折算 */
    std::size_t m_baseMaxFrames = 8; /**< 最深帧数基准，按质量档位折算 */
};

// ==================== 自适应控制器 ====================

// 纯策略类：不持有帧、不加锁、不打日志。它只做一件事——把观测换算成
// 三个数：目标延迟、最长等待、最深帧数。真正的缓冲和出帧判定在收流循环里。
//
// 日志交给调用方：模式或质量发生变化时，调用方比较 recordJitter() 前后的
// activeMode() / networkQuality() 决定是否记一条。策略类里埋 Logger 会把
// 一个可以直接单测的纯逻辑绑死在日志模块上。
class AdaptiveJitterBuffer {
public:
    AdaptiveJitterBuffer() = default;

    // 写入配置，并把当前等待/帧数上限重置为配置基准值。
    void configure(const AdaptiveJitterBufferConfig &config) {
        m_config = config;
        m_currentMaxWaitMs = m_config.m_baseMaxWaitMs;
        m_currentMaxFrames = m_config.m_baseMaxFrames;
    }

    // 只关心抖动、不关心丢包与跳帧时用这个重载（后两者按 0 计）。
    void recordJitter(double jitterMs, std::uint64_t nowNs) {
        recordJitter(jitterMs, 0.0, 0.0, nowNs);
    }

    // 每收到一帧调用一次，驱动整套策略。
    void recordJitter(double jitterMs,
                      double fragmentLossPercent,
                      double skipRatePercent,
                      std::uint64_t nowNs) {
        m_window.record(jitterMs);
        assessNetworkQuality(fragmentLossPercent, skipRatePercent);

        // 固定模式：不计数、不切换、也不按质量调参。
        if (!m_autoMode) {
            return;
        }

        // 连续次数每个样本都累计，不受下面的样本数门槛约束：门槛的含义是
        // 「P95 还没统计意义，先别做判定」，不该把连续计数一起吞掉——否则
        // 「连续 10 次」实际要等到第 19 个样本才生效，量出来的反应时间是错的。
        trackJitterStreak(jitterMs);

         if (m_window.sampleCount() < kAutoModeMinSamples) {
            return;
        }
        evaluateStateTransition(nowNs);
        applyQualityParams();
    }

    // 对外统一纳秒：调用方算截止时刻不用再做单位换算。
    std::uint64_t targetDelayNs() const {
        return computeDelayForMode(m_activeMode);
    }

    // 固定模式：关闭自动切换，也不按质量调整等待/帧数上限。
    // 注意目标延迟仍随 P95 变化——固定的是策略档位，不是延迟数值。
    void setFixedMode(JitterBufferMode mode) {
        m_autoMode = false;
        m_activeMode = mode;
    }

    // 自动模式：根据质量自动切换策略档位，按质量调整等待/帧数上限。
    void enableAutoMode() {
        m_autoMode = true;
    }
    // 重置所有状态，包括窗口、质量评级、连续次数等。
    void reset() {
        m_window = sclient::base::LatencyStats(kJitterWindowSamples, kJitterSnapshotRefreshInterval);
        m_activeMode = JitterBufferMode::Bypass;
        m_networkQuality = NetworkQuality::Excellent;
        m_consecutiveHighJitter = 0;
        m_consecutiveLowJitter = 0;
        m_lowToSmoothP95Exceeded = false;
        m_smoothLowP95SinceNs = 0;
        m_currentMaxWaitMs = m_config.m_baseMaxWaitMs;
        m_currentMaxFrames = m_config.m_baseMaxFrames;
    }
    // 获取当前策略档位、质量评级、自动模式状态。
    JitterBufferMode activeMode() const { return m_activeMode; }
    // 获取当前质量评级。
    NetworkQuality networkQuality() const { return m_networkQuality; }
    // 获取当前自动模式状态。
    bool autoMode() const { return m_autoMode; }

    // 获取当前窗口动统计
    double jitterP50Ms() const { return m_window.snapshot().m_p50Ms; }
    double jitterP95Ms() const { return m_window.snapshot().m_p95Ms; }
    double jitterP99Ms() const { return m_window.snapshot().m_p99Ms; }
    std::size_t jitterSamples() const { return m_window.sampleCount(); }

    // 获取当前质量档位的等待上限与帧深上限。
    int qualityMaxWaitMs() const { return m_currentMaxWaitMs; }
    std::size_t qualityMaxFrames() const { return m_currentMaxFrames; }

private:
    // 质量评级只看窗口 P95，不看单次抖动：一次毛刺不该让整条链路降级。
    void assessNetworkQuality(double lossPercent, double skipPercent) {
        const double kP95 = jitterP95Ms();

        // 质量评级判断逻辑：根据窗口 P95、丢包率、跳帧率判断。
        if (kP95 > kQualityPoorP95Ms ||
            lossPercent > kQualityPoorLossPercent ||
            skipPercent > kQualityPoorSkipPercent) {
            m_networkQuality = NetworkQuality::Poor;
        } else if (kP95 > kQualityFairP95Ms ||
                   lossPercent > kQualityFairLossPercent ||
                   skipPercent > kQualityFairSkipPercent) {
            m_networkQuality = NetworkQuality::Fair;
        } else if (kP95 > kQualityGoodP95Ms || lossPercent > kQualityGoodLossPercent) {
            m_networkQuality = NetworkQuality::Good;
        } else {
            m_networkQuality = NetworkQuality::Excellent;
        }
    }

    // 质量档位决定等待与深度上限。
    //
    // 恶化方向按每次最多约 1.5 倍逐级爬升：从 80ms 一步跳到 240ms 会让用户
    // 直接感觉到延迟突变，而逐级爬升在两三帧内就完成了，用户察觉不到。
    // 恢复方向立即生效，尽快把延迟降回来——降延迟没有副作用。
    void applyQualityParams() {
        int targetWaitMs = 0;
        std::size_t targetFrames = 0;
        switch (m_networkQuality) {
            case NetworkQuality::Excellent:
                targetWaitMs = std::max(20, m_config.m_baseMaxWaitMs / 4);
                targetFrames = std::max<std::size_t>(2, m_config.m_baseMaxFrames / 4);
                break;
            case NetworkQuality::Good:
                targetWaitMs = m_config.m_baseMaxWaitMs / 2;
                targetFrames = std::max<std::size_t>(4, m_config.m_baseMaxFrames / 2);
                break;
            case NetworkQuality::Fair:
                targetWaitMs = m_config.m_baseMaxWaitMs;
                targetFrames = m_config.m_baseMaxFrames;
                break;
            case NetworkQuality::Poor:
            default:
                targetWaitMs = m_config.m_baseMaxWaitMs * 3;
                targetFrames = m_config.m_baseMaxFrames * 4;
                break;
        }

        // 质量档位决定等待与深度上限。
        if (targetWaitMs > m_currentMaxWaitMs) {
            const int kGrowthMs = std::max(5, m_currentMaxWaitMs / 2);
            m_currentMaxWaitMs = std::min(targetWaitMs, m_currentMaxWaitMs + kGrowthMs);
        } else {
            m_currentMaxWaitMs = targetWaitMs;
        }

        if (targetFrames > m_currentMaxFrames) {
            const std::size_t kGrowthFrames = std::max<std::size_t>(1, m_currentMaxFrames / 2);
            m_currentMaxFrames = std::min(targetFrames, m_currentMaxFrames + kGrowthFrames);
        } else {
            m_currentMaxFrames = targetFrames;
        }
    }

    // 按当前模式维护「连续满足条件的样本数」。出现一个反向样本就清零——
    // 这正是「连续」与「累计」的区别，也是抗毛刺的全部来源。
    void trackJitterStreak(double jitterMs) {
        switch (m_activeMode) {
            case JitterBufferMode::Bypass:
                m_consecutiveHighJitter = jitterMs > kBypassToLowLatencyJitterMs
                        ? m_consecutiveHighJitter + 1
                        : 0;
                break;
            case JitterBufferMode::LowLatency:
                m_consecutiveLowJitter = jitterMs < kLowLatencyToBypassJitterMs
                        ? m_consecutiveLowJitter + 1
                        : 0;
                break;
            case JitterBufferMode::Smooth:
                // Smooth 的降档由 P95 持续时长决定，不靠逐样本计数。
                m_consecutiveHighJitter = 0;
                m_consecutiveLowJitter = 0;
                break;
        }
    }


    // 四条迁移规则（阈值见文件顶部常量）：
    //   Bypass     → LowLatency：连续 10 次 jitter > 1ms
    //   LowLatency → Bypass    ：连续 20 次 jitter < 0.5ms
    //   LowLatency → Smooth    ：P95 连续 2 次 > 10ms
    //   Smooth     → LowLatency：P95 持续 < 5ms 达 3 秒
    //
    // 两个方向的判据故意不同：快速档位迁移看单次抖动（要反应快），
    // 慢速档位迁移看窗口 P95（要平滑）。混用是有意的，不是疏漏。
    void evaluateStateTransition(std::uint64_t nowNs) {
        const double kP95 = jitterP95Ms();

        switch (m_activeMode) {
            case JitterBufferMode::Bypass:
                if (m_consecutiveHighJitter >= kBypassToLowLatencySamples) {
                    m_activeMode = JitterBufferMode::LowLatency;
                    m_consecutiveHighJitter = 0;
                    m_consecutiveLowJitter = 0;
                    m_lowToSmoothP95Exceeded = false;
                }
                break;

            case JitterBufferMode::LowLatency:
                if (m_consecutiveLowJitter >= kLowLatencyToBypassSamples) {
                    m_activeMode = JitterBufferMode::Bypass;
                    m_consecutiveLowJitter = 0;
                    m_consecutiveHighJitter = 0;
                    // 已经降到 Bypass，本轮不再评估升级，避免同一帧里连跳两档。
                    break;
                }
                // 用一个 bool 实现「连续两次」：第一次超阈值置位，第二次仍
                // 超才迁移；中间任意一次不超就清零。
                if (kP95 > kLowLatencyToSmoothP95Ms) {
                    if (m_lowToSmoothP95Exceeded) {
                        m_activeMode = JitterBufferMode::Smooth;
                        m_consecutiveLowJitter = 0;
                        m_consecutiveHighJitter = 0;
                        m_lowToSmoothP95Exceeded = false;
                        m_smoothLowP95SinceNs = 0;
                    } else {
                        m_lowToSmoothP95Exceeded = true;
                    }
                } else {
                    m_lowToSmoothP95Exceeded = false;
                }
                break;

            case JitterBufferMode::Smooth:
                // 记录 P95 首次低于阈值的时刻，持续够久才降档；中途回升即清零。
                if (kP95 < kSmoothToLowLatencyP95Ms) {
                    if (m_smoothLowP95SinceNs == 0) {
                        m_smoothLowP95SinceNs = nowNs;
                    } else if (nowNs - m_smoothLowP95SinceNs >= kSmoothDowngradeHoldNs) {
                        m_activeMode = JitterBufferMode::LowLatency;
                        m_consecutiveLowJitter = 0;
                        m_consecutiveHighJitter = 0;
                        m_lowToSmoothP95Exceeded = false;
                        m_smoothLowP95SinceNs = 0;
                    }
                } else {
                    m_smoothLowP95SinceNs = 0;
                }
                break;
        }
    }

    // 目标延迟 = P95 × 安全系数，夹进 [minDelayMs, maxDelayMs]。
    // 上限的意义是硬性天花板：抖动大到 200ms 时也不能让延迟无限增长，
    // 那时宁可跳帧。
    std::uint64_t computeDelayForMode(JitterBufferMode mode) const {
        switch (mode) {
            case JitterBufferMode::Bypass:
                return 0;

            case JitterBufferMode::LowLatency: {
                const double kTarget = std::clamp(
                        jitterP95Ms() * m_config.m_safetyFactor,
                        static_cast<double>(m_config.m_minDelayMs),
                        static_cast<double>(std::max(m_config.m_maxDelayMs, m_config.m_minDelayMs)));
                return static_cast<std::uint64_t>(kTarget * 1000000.0);
            }

            case JitterBufferMode::Smooth: {
                const double kTarget = std::clamp(
                        jitterP95Ms() * m_config.m_smoothFactor,
                        static_cast<double>(m_config.m_minDelayMs),
                        static_cast<double>(std::max(m_config.m_maxDelayMs, m_config.m_minDelayMs)));
                return static_cast<std::uint64_t>(kTarget * 1000000.0);
            }
        }
        return 0;
    }

    // 核心状态：当前模式、质量档位、是否允许自动切换。
    JitterBufferMode m_activeMode = JitterBufferMode::Bypass;
    NetworkQuality m_networkQuality = NetworkQuality::Excellent;
    bool m_autoMode = true;
    AdaptiveJitterBufferConfig m_config;
    sclient::base::LatencyStats m_window{kJitterWindowSamples, kJitterSnapshotRefreshInterval};

    // 迟滞判定状态：高/低抖动各数各的连续次数，出现反向样本即清零。
    int m_consecutiveHighJitter = 0;
    int m_consecutiveLowJitter = 0;
    bool m_lowToSmoothP95Exceeded = false;
    std::uint64_t m_smoothLowP95SinceNs = 0;

    // 按质量档位折算出的等待与深度上限，由 applyQualityParams() 维护。
    int m_currentMaxWaitMs = 80;
    std::size_t m_currentMaxFrames = 8;
};


}// namespace transport
}// namespace sclient


#endif