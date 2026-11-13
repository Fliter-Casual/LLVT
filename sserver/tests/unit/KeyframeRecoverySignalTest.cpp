// KeyframeRecoverySignal：请求、取走即清与冷却窗口的单元测试。
//
// 时间由用例显式传入，不依赖真实时钟。
#include <cstdint>

#include "sserver/base/KeyframeRecoverySignal.h"
#include "support/Check.h"

namespace {

using sserver::base::KeyframeRecoverySignal;

constexpr std::uint64_t kMs = 1000000ULL;
constexpr std::uint64_t kCooldownMs = 500;

void testRequestIsConsumedOnce() {
    KeyframeRecoverySignal signal(kCooldownMs * kMs);
    CHECK(!signal.pending());

    signal.request();
    CHECK(signal.pending());
    CHECK(signal.consume(0));
    // 取走即清：同一帧不能被两个地方当成"需要关键帧"处理。
    CHECK(!signal.pending());
    CHECK(!signal.consume(0));
}

void testCooldownBlocksRepeatGrants() {
    KeyframeRecoverySignal signal(kCooldownMs * kMs);
    signal.request();
    CHECK(signal.consume(0));

    // 冷却期内的请求不授予，但**保留**——否则恰好落在窗口里的那次
    // 丢帧就永远得不到补偿。
    signal.request();
    CHECK(!signal.consume(100 * kMs));
    CHECK(!signal.consume(499 * kMs));
    CHECK(signal.pending());
    CHECK(signal.consume(500 * kMs));
    CHECK(!signal.pending());
}

void testFirstGrantAtZeroTimestamp() {
    // monotonicNowNs 在进程刚启动时可能就是 0，不能被当成"从未授予"
    // 的哨兵值而拒绝首次授权。
    KeyframeRecoverySignal signal(kCooldownMs * kMs);
    signal.request();
    CHECK(signal.consume(0));

    signal.request();
    CHECK(!signal.consume(1));
}

void testRepeatedRequestsCollapse() {
    KeyframeRecoverySignal signal(kCooldownMs * kMs);
    // 多个会话同时丢帧会连发多次请求，但只应产生一个 IDR。
    signal.request();
    signal.request();
    signal.request();
    CHECK(signal.consume(0));
    CHECK(!signal.consume(0));
}

void testZeroCooldownGrantsEveryTime() {
    KeyframeRecoverySignal signal(0);
    signal.request();
    CHECK(signal.consume(1000));
    signal.request();
    CHECK(signal.consume(1000));
}

}  // namespace

int main() {
    testRequestIsConsumedOnce();
    testCooldownBlocksRepeatGrants();
    testFirstGrantAtZeroTimestamp();
    testRepeatedRequestsCollapse();
    testZeroCooldownGrantsEveryTime();
    return sserver::test::finish("keyframe_recovery_signal_test");
}
