// LatencyStats：环形窗口、分位数、节流刷新与容量为 0 的单元测试。
//
// 分位数期望值手算，不与实现共用公式。样本 1/2/3/4 ms（n=4）：
//   p50 = 位置 1.5  → 2.5
//   p95 = 位置 2.85 → 3.85
//   p99 = 位置 2.97 → 3.97
#include <string>

#include "sclient/base/LatencyStats.h"
#include "support/Check.h"

namespace {

using sclient::base::LatencyStats;
using sclient::base::LatencySummary;

bool approx(double lhs, double rhs) {
    const double kDifference = lhs - rhs;
    return (kDifference < 0.0 ? -kDifference : kDifference) < 1e-6;
}

void testEmpty() {
    LatencyStats stats;
    const LatencySummary summary = stats.snapshot();
    CHECK(summary.m_count == 0);
    CHECK(approx(summary.m_p95Ms, 0.0));
    CHECK(!stats.hasSamples());
}

void testKnownPercentiles() {
    // 刷新间隔设为 1，让每次 snapshot 都重算，先验证纯统计逻辑。
    LatencyStats stats(100, 1);
    stats.record(1.0);
    stats.record(2.0);
    stats.record(3.0);
    stats.record(4.0);

    const LatencySummary summary = stats.snapshot();
    CHECK(summary.m_count == 4);
    CHECK(approx(summary.m_minMs, 1.0));
    CHECK(approx(summary.m_maxMs, 4.0));
    CHECK(approx(summary.m_avgMs, 2.5));
    CHECK(approx(summary.m_p50Ms, 2.5));
    CHECK(approx(summary.m_p95Ms, 3.85));
    CHECK(approx(summary.m_p99Ms, 3.97));
    CHECK(approx(summary.m_lastMs, 4.0));
}

void testSingleSample() {
    LatencyStats stats(100, 1);
    stats.record(5.0);

    const LatencySummary summary = stats.snapshot();
    CHECK(summary.m_count == 1);
    CHECK(approx(summary.m_minMs, 5.0));
    CHECK(approx(summary.m_p95Ms, 5.0));
    CHECK(approx(summary.m_maxMs, 5.0));
}

void testRingWrapKeepsMostRecent() {
    // 容量 4，写 6 个样本：窗口里应只剩 3/4/5/6。
    LatencyStats stats(4, 1);
    for (int value = 1; value <= 6; ++value) {
        stats.record(static_cast<double>(value));
    }

    const LatencySummary summary = stats.snapshot();
    CHECK(summary.m_count == 4);
    CHECK(approx(summary.m_minMs, 3.0));
    CHECK(approx(summary.m_maxMs, 6.0));
    CHECK(approx(summary.m_avgMs, 4.5));
}

void testSnapshotIsThrottled() {
    // 刷新间隔 4：第一次查询建缓存，之后每 4 个新样本才重算一次。
    LatencyStats stats(100, 4);
    stats.record(1.0);
    CHECK(stats.snapshot().m_count == 1);   // 首次：缓存无效，立即重算

    stats.record(2.0);
    CHECK(stats.snapshot().m_count == 1);   // 已累积 1 个，未到间隔
    stats.record(3.0);
    CHECK(stats.snapshot().m_count == 1);
    stats.record(4.0);
    CHECK(stats.snapshot().m_count == 1);   // 已累积 3 个，仍不到

    stats.record(5.0);
    CHECK(stats.snapshot().m_count == 5);   // 累积满 4 个，重算
}

void testFormatForcesRefresh() {
    LatencyStats stats(100, 8);
    stats.record(1.0);
    CHECK(stats.snapshot().m_count == 1);

    stats.record(2.0);
    // 节流窗口内 snapshot 仍是旧值，但 format 必须给出最新数据：
    // 日志里出现过期的 count 会直接误导排查。
    CHECK(stats.snapshot().m_count == 1);
    const std::string text = stats.format("jitter");
    CHECK(text.find("count=2") != std::string::npos);
    CHECK(text.find("jitter") != std::string::npos);
    CHECK(text.find("last=2.00ms") != std::string::npos);
}

void testZeroCapacityKeepsOnlyLast() {
    LatencyStats stats(0, 4);
    stats.record(7.5);

    CHECK(!stats.hasSamples());
    CHECK(stats.sampleCount() == 0);
    CHECK(approx(stats.lastMs(), 7.5));
    CHECK(stats.snapshot().m_count == 0);
}

void testReset() {
    LatencyStats stats(4, 1);
    stats.record(1.0);
    stats.record(2.0);
    CHECK(stats.hasSamples());

    stats.reset();
    CHECK(!stats.hasSamples());
    CHECK(approx(stats.lastMs(), 0.0));
    CHECK(stats.snapshot().m_count == 0);
}

}  // namespace

int main() {
    testEmpty();
    testKnownPercentiles();
    testSingleSample();
    testRingWrapKeepsMostRecent();
    testSnapshotIsThrottled();
    testFormatForcesRefresh();
    testZeroCapacityKeepsOnlyLast();
    testReset();
    return sclient::test::finish("latency_stats_test");
}
