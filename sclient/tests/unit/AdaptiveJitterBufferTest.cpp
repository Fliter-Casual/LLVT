// AdaptiveJitterBuffer：质量分级、模式迟滞切换、目标延迟与等待上限的单元测试。
//
// 全部用显式传入的 nowNs，不依赖真实时钟，因此切换时序完全确定。
// 注意：抖动窗口容量 100、每 4 个样本刷新一次快照，P95 的反应有最多
// 4 个样本的滞后——凡是依赖 P95 的迁移都写成「循环直到切换」并检查
// 切换发生在合理区间内，而不是断言精确的样本序号。
#include <cstdint>

#include "sclient/transport/udp/AdaptiveJitterBuffer.h"
#include "sclient/transport/udp/JitterBufferTypes.h"
#include "support/Check.h"

namespace {

using sclient::transport::AdaptiveJitterBuffer;
using sclient::transport::AdaptiveJitterBufferConfig;
using sclient::transport::JitterBufferMode;
using sclient::transport::NetworkQuality;

constexpr std::uint64_t kMs = 1000000ULL;

bool approx(double lhs, double rhs) {
    const double kDifference = lhs - rhs;
    return (kDifference < 0.0 ? -kDifference : kDifference) < 1e-6;
}

// 连续灌入同样大小的抖动样本，返回最后一个使用的时间戳。
std::uint64_t feed(AdaptiveJitterBuffer &buffer, double jitterMs, int count,
                   std::uint64_t nowNs, std::uint64_t stepNs) {
    for (int index = 0; index < count; ++index) {
        buffer.recordJitter(jitterMs, nowNs);
        nowNs += stepNs;
    }
    return nowNs;
}

void testInitialState() {
    AdaptiveJitterBuffer buffer;
    CHECK(buffer.activeMode() == JitterBufferMode::Bypass);
    CHECK(buffer.networkQuality() == NetworkQuality::Excellent);
    CHECK(buffer.autoMode());
    CHECK(buffer.targetDelayNs() == 0);  // 旁路：目标缓冲延迟为零
    CHECK(buffer.jitterSamples() == 0);
}

void testBypassToLowLatencyNeedsTenConsecutiveSamples() {
    AdaptiveJitterBuffer buffer;
    std::uint64_t nowNs = 0;

    nowNs = feed(buffer, 2.0, 9, nowNs, kMs);
    // 第九次还不够：门槛是连续 10 次，差一次都不切。
    CHECK(buffer.activeMode() == JitterBufferMode::Bypass);

    buffer.recordJitter(2.0, nowNs);
    CHECK(buffer.activeMode() == JitterBufferMode::LowLatency);
}

void testLowLatencyToBypassNeedsTwentyConsecutiveSamples() {
    AdaptiveJitterBuffer buffer;
    std::uint64_t nowNs = feed(buffer, 2.0, 10, 0, kMs);
    CHECK(buffer.activeMode() == JitterBufferMode::LowLatency);

    nowNs = feed(buffer, 0.1, 19, nowNs, kMs);
    // 降档门槛比升档高（20 次 vs 10 次）：宁可多缓冲一会，也别在
    // 临界网络上来回切换。
    CHECK(buffer.activeMode() == JitterBufferMode::LowLatency);

    buffer.recordJitter(0.1, nowNs);
    CHECK(buffer.activeMode() == JitterBufferMode::Bypass);
}

void testLowLatencyToSmoothOnSustainedHighP95() {
    AdaptiveJitterBuffer buffer;
    std::uint64_t nowNs = feed(buffer, 2.0, 10, 0, kMs);
    CHECK(buffer.activeMode() == JitterBufferMode::LowLatency);

    // 抖动升到 20ms：P95 超过 10ms 需要连续两次才升档。
    int iterations = 0;
    while (buffer.activeMode() == JitterBufferMode::LowLatency && iterations < 400) {
        buffer.recordJitter(20.0, nowNs);
        nowNs += kMs;
        ++iterations;
    }
    CHECK(buffer.activeMode() == JitterBufferMode::Smooth);
    // 不该是一两个样本就切：要等窗口 P95 刷新。
    CHECK(iterations > 2);
}

void testSmoothToLowLatencyRequiresThreeSecondHold() {
    AdaptiveJitterBuffer buffer;
    // 直接进入 Smooth：固定模式下不参与自动切换，再打开自动模式。
    buffer.setFixedMode(JitterBufferMode::Smooth);
    buffer.enableAutoMode();
    CHECK(buffer.activeMode() == JitterBufferMode::Smooth);

    // 先用高抖动把窗口填满，P95 保持高位。
    std::uint64_t nowNs = feed(buffer, 20.0, 12, 0, kMs);

    // 再喂低抖动，直到窗口把旧样本全部挤出去、P95 掉到 5ms 以下并持续 3 秒。
    int iterations = 0;
    while (buffer.activeMode() == JitterBufferMode::Smooth && iterations < 1000) {
        buffer.recordJitter(0.05, nowNs);
        nowNs += 100 * kMs;
        ++iterations;
    }
    CHECK(buffer.activeMode() == JitterBufferMode::LowLatency);
    // 至少要先冲掉 100 个样本的窗口，再加上 3 秒保持时间（100ms 步长 → 30 次）。
    CHECK(iterations >= 120);
}

void testFixedModeDisablesSwitching() {
    AdaptiveJitterBuffer buffer;
    buffer.setFixedMode(JitterBufferMode::Bypass);
    CHECK(!buffer.autoMode());

    // 固定模式下即使抖动很大也不切换，也不按质量调整等待上限。
    feed(buffer, 20.0, 500, 0, kMs);
    CHECK(buffer.activeMode() == JitterBufferMode::Bypass);

    AdaptiveJitterBufferConfig config;
    buffer.configure(config);
    feed(buffer, 60.0, 500, 0, kMs);
    CHECK(buffer.qualityMaxWaitMs() == config.m_baseMaxWaitMs);
    CHECK(buffer.qualityMaxFrames() == config.m_baseMaxFrames);
}

void testTargetDelayUsesP95TimesSafetyFactor() {
    // 固定 LowLatency，P95 = 2ms、系数 1.5 → 目标 3ms。
    AdaptiveJitterBuffer fixedLow;
    fixedLow.setFixedMode(JitterBufferMode::LowLatency);
    feed(fixedLow, 2.0, 12, 0, kMs);
    CHECK(approx(static_cast<double>(fixedLow.targetDelayNs()) / 1e6, 3.0));

    // 固定 Smooth，P95 = 20ms、系数 2.5 → 目标 50ms。
    AdaptiveJitterBuffer fixedSmooth;
    fixedSmooth.setFixedMode(JitterBufferMode::Smooth);
    feed(fixedSmooth, 20.0, 12, 0, kMs);
    CHECK(approx(static_cast<double>(fixedSmooth.targetDelayNs()) / 1e6, 50.0));
}

void testTargetDelayClampBounds() {
    AdaptiveJitterBufferConfig config;  // 下限 2ms、上限 120ms

    // 抖动极小：P95*1.5 低于下限，被夹到 2ms。
    AdaptiveJitterBuffer low;
    low.configure(config);
    low.setFixedMode(JitterBufferMode::LowLatency);
    feed(low, 0.1, 12, 0, kMs);
    CHECK(approx(static_cast<double>(low.targetDelayNs()) / 1e6, 2.0));

    // 抖动极大：P95*2.5 = 250ms 超过硬上限，被夹到 120ms。
    AdaptiveJitterBuffer high;
    high.configure(config);
    high.setFixedMode(JitterBufferMode::Smooth);
    feed(high, 100.0, 12, 0, kMs);
    CHECK(approx(static_cast<double>(high.targetDelayNs()) / 1e6, 120.0));
}

void testNetworkQualityThresholds() {
    struct Case {
        double m_jitterMs;
        NetworkQuality m_expected;
    };
    const Case kCases[] = {
        {0.0, NetworkQuality::Excellent},
        {2.0, NetworkQuality::Good},
        {20.0, NetworkQuality::Fair},
        {60.0, NetworkQuality::Poor},
    };

    for (const Case &item : kCases) {
        AdaptiveJitterBuffer buffer;
        feed(buffer, item.m_jitterMs, 12, 0, kMs);
        CHECK(buffer.networkQuality() == item.m_expected);
    }
}

void testQualityParamsGrowGradually() {
    // 基准 80ms / 8 帧；Poor 目标 240ms / 32 帧。
    AdaptiveJitterBuffer buffer;
    std::uint64_t nowNs = feed(buffer, 60.0, 10, 0, kMs);
    CHECK(buffer.networkQuality() == NetworkQuality::Poor);

    // 恶化方向按 ~1.5 倍逐级爬升：第一次只到 120ms / 12 帧，
    // 不是一步跳到 240ms。突然的 160ms 延迟增量用户是能感觉到的。
    CHECK(buffer.qualityMaxWaitMs() == 120);
    CHECK(buffer.qualityMaxFrames() == 12);

    // 继续喂同样的抖动，上限应当单调增长并最终收敛到目标值。
    int previousWait = buffer.qualityMaxWaitMs();
    for (int index = 0; index < 20; ++index) {
        buffer.recordJitter(60.0, nowNs);
        nowNs += kMs;
        CHECK(buffer.qualityMaxWaitMs() >= previousWait);
        previousWait = buffer.qualityMaxWaitMs();
    }
    CHECK(buffer.qualityMaxWaitMs() == 240);
    CHECK(buffer.qualityMaxFrames() == 32);
}

void testQualityParamsRecoverImmediately() {
    AdaptiveJitterBuffer buffer;
    std::uint64_t nowNs = feed(buffer, 60.0, 40, 0, kMs);
    CHECK(buffer.qualityMaxWaitMs() == 240);

    // 恢复方向立即生效：延迟降下来没有副作用，不需要渐进。
    // 但要等窗口里的高抖动样本被挤出去——P95 是分位数，不是滑动平均，
    // 40 个 60ms 样本必须全部离开 100 样本窗口，P95 才会掉下来。
    feed(buffer, 0.0, 100, nowNs, kMs);
    CHECK(buffer.networkQuality() == NetworkQuality::Excellent);
    CHECK(buffer.qualityMaxWaitMs() == 20);
    CHECK(buffer.qualityMaxFrames() == 2);
}

void testResetRestoresDefaults() {
    AdaptiveJitterBuffer buffer;
    feed(buffer, 60.0, 40, 0, kMs);
    CHECK(buffer.activeMode() != JitterBufferMode::Bypass);
    CHECK(buffer.jitterSamples() > 0);

    buffer.reset();
    CHECK(buffer.activeMode() == JitterBufferMode::Bypass);
    CHECK(buffer.networkQuality() == NetworkQuality::Excellent);
    CHECK(buffer.jitterSamples() == 0);
    CHECK(buffer.qualityMaxWaitMs() == 80);
    CHECK(buffer.qualityMaxFrames() == 8);
}

void testSkipRateUsesOutcomesNotEmitted() {
    using sclient::transport::jitterBufferSkipRatePercent;
    CHECK(approx(jitterBufferSkipRatePercent(0, 0), 0.0));
    CHECK(approx(jitterBufferSkipRatePercent(3, 1), 25.0));
    // 分母含跳帧：发 1 跳 3 是 75%，而不是 300%。
    CHECK(approx(jitterBufferSkipRatePercent(1, 3), 75.0));
}

}  // namespace

int main() {
    testInitialState();
    testBypassToLowLatencyNeedsTenConsecutiveSamples();
    testLowLatencyToBypassNeedsTwentyConsecutiveSamples();
    testLowLatencyToSmoothOnSustainedHighP95();
    testSmoothToLowLatencyRequiresThreeSecondHold();
    testFixedModeDisablesSwitching();
    testTargetDelayUsesP95TimesSafetyFactor();
    testTargetDelayClampBounds();
    testNetworkQualityThresholds();
    testQualityParamsGrowGradually();
    testQualityParamsRecoverImmediately();
    testResetRestoresDefaults();
    testSkipRateUsesOutcomesNotEmitted();
    return sclient::test::finish("adaptive_jitter_buffer_test");
}
