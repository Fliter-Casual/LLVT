// LatencyRecorder：滑动窗口、分位数插值、级联聚合与并发写入的单元测试。
//
// 分位数的期望值是手算的，不与实现共用公式——否则公式写错时测试跟着错。
// 样本 1/2/3/4 ms（n=4）：
//   p50 = 位置 1.5  → 2 与 3 之间 → 2.5
//   p95 = 位置 2.85 → 3 与 4 之间 → 3.85
//   p99 = 位置 2.97 → 3 与 4 之间 → 3.97
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sserver/base/LatencyRecorder.h"
#include "support/Check.h"

namespace {

using sserver::base::LatencyRecorder;
using sserver::base::LatencySummary;

std::uint64_t msToNs(std::uint64_t milliseconds) {
    return milliseconds * 1000000ULL;
}

bool approx(double lhs, double rhs) {
    const double kDifference = lhs - rhs;
    return (kDifference < 0.0 ? -kDifference : kDifference) < 1e-6;
}

void testEmptySummary() {
    LatencyRecorder recorder;
    const LatencySummary summary = recorder.snapshot();
    CHECK(summary.m_count == 0);
    CHECK(approx(summary.m_minMs, 0.0));
    CHECK(approx(summary.m_maxMs, 0.0));
    CHECK(approx(summary.m_avgMs, 0.0));
}

void testKnownPercentiles() {
    LatencyRecorder recorder;
    recorder.recordNs(msToNs(1));
    recorder.recordNs(msToNs(2));
    recorder.recordNs(msToNs(3));
    recorder.recordNs(msToNs(4));

    const LatencySummary summary = recorder.snapshot();
    CHECK(summary.m_count == 4);
    CHECK(approx(summary.m_minMs, 1.0));
    CHECK(approx(summary.m_maxMs, 4.0));
    CHECK(approx(summary.m_avgMs, 2.5));
    CHECK(approx(summary.m_p50Ms, 2.5));
    CHECK(approx(summary.m_p95Ms, 3.85));
    CHECK(approx(summary.m_p99Ms, 3.97));
}

void testSingleSample() {
    LatencyRecorder recorder;
    recorder.recordNs(msToNs(5));

    // 单样本时所有分位数都必须退化成该样本本身。
    const LatencySummary summary = recorder.snapshot();
    CHECK(summary.m_count == 1);
    CHECK(approx(summary.m_minMs, 5.0));
    CHECK(approx(summary.m_avgMs, 5.0));
    CHECK(approx(summary.m_p50Ms, 5.0));
    CHECK(approx(summary.m_p95Ms, 5.0));
    CHECK(approx(summary.m_p99Ms, 5.0));
    CHECK(approx(summary.m_maxMs, 5.0));
}

void testZeroLatencyIsValidSample() {
    // 0 纳秒是合法样本，不能被当作「没有数据」丢掉。
    LatencyRecorder recorder;
    recorder.recordNs(0);
    recorder.recordNs(msToNs(2));

    const LatencySummary summary = recorder.snapshot();
    CHECK(summary.m_count == 2);
    CHECK(approx(summary.m_minMs, 0.0));
    CHECK(approx(summary.m_maxMs, 2.0));
}

void testSlidingWindowDropsOldest() {
    LatencyRecorder recorder(4);
    for (std::uint64_t value = 1; value <= 6; ++value) {
        recorder.recordNs(msToNs(value));
    }

    // 容量 4，窗口里只剩 3/4/5/6。
    const LatencySummary summary = recorder.snapshot();
    CHECK(summary.m_count == 4);
    CHECK(approx(summary.m_minMs, 3.0));
    CHECK(approx(summary.m_maxMs, 6.0));
}

void testZeroCapacityForwardsOnly() {
    // 容量 0 = 纯转发节点：本地不留样本，只向上聚合。
    auto parent = std::make_shared<LatencyRecorder>();
    LatencyRecorder child(0, parent);
    child.recordNs(msToNs(7));

    CHECK(child.snapshot().m_count == 0);
    CHECK(parent->snapshot().m_count == 1);
    CHECK(approx(parent->snapshot().m_maxMs, 7.0));
}

void testAggregateCascade() {
    auto global = std::make_shared<LatencyRecorder>();
    LatencyRecorder sessionA(64, global);
    LatencyRecorder sessionB(64, global);

    sessionA.recordNs(msToNs(10));
    sessionB.recordNs(msToNs(20));

    CHECK(sessionA.snapshot().m_count == 1);
    CHECK(sessionB.snapshot().m_count == 1);
    CHECK(global->snapshot().m_count == 2);
    CHECK(approx(global->snapshot().m_maxMs, 20.0));
}

void testResetIsLocalOnly() {
    auto global = std::make_shared<LatencyRecorder>();
    LatencyRecorder session(64, global);
    session.recordNs(msToNs(3));
    CHECK(global->snapshot().m_count == 1);

    session.reset();
    CHECK(session.snapshot().m_count == 0);
    // 聚合方汇总的是所有会话的数据，单个会话复位不能把别人一起清掉。
    CHECK(global->snapshot().m_count == 1);
}

void testFormatContainsAllFields() {
    LatencyRecorder recorder;
    recorder.recordNs(msToNs(1));
    recorder.recordNs(msToNs(2));
    recorder.recordNs(msToNs(3));
    recorder.recordNs(msToNs(4));

    const std::string text = recorder.format("encode");
    CHECK(text.find("encode") != std::string::npos);
    CHECK(text.find("count=4") != std::string::npos);
    CHECK(text.find("p50=2.50ms") != std::string::npos);
    CHECK(text.find("p95=3.85ms") != std::string::npos);
    CHECK(text.find("max=4.00ms") != std::string::npos);
}

void testConcurrentWriters() {
    constexpr std::size_t kCapacity = 100;
    constexpr int kThreads = 4;
    constexpr int kSamplesPerThread = 50;

    LatencyRecorder recorder(kCapacity);
    std::vector<std::thread> writers;
    writers.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        writers.emplace_back([&recorder, index]() {
            for (int sample = 0; sample < kSamplesPerThread; ++sample) {
                recorder.recordNs(msToNs(static_cast<std::uint64_t>(index + 1)));
            }
        });
    }
    for (auto &writer : writers) {
        writer.join();
    }

    // 样本总数 200 超过容量 100，窗口必须正好停在容量上限。
    CHECK(recorder.snapshot().m_count == kCapacity);
}

}  // namespace

int main() {
    testEmptySummary();
    testKnownPercentiles();
    testSingleSample();
    testZeroLatencyIsValidSample();
    testSlidingWindowDropsOldest();
    testZeroCapacityForwardsOnly();
    testAggregateCascade();
    testResetIsLocalOnly();
    testFormatContainsAllFields();
    testConcurrentWriters();
    return sserver::test::finish("latency_recorder_test");
}