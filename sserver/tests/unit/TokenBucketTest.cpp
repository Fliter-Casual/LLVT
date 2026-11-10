// TokenBucket：整形额度的补充、封顶与等待估算的单元测试。
//
// 时间由用例显式传入，所以不依赖真实时钟、也不会因为机器慢而抖动。
#include <cstddef>
#include <cstdint>

#include "sserver/transport/udp/TokenBucket.h"
#include "support/Check.h"

namespace {

using sserver::transport::TokenBucket;

constexpr std::uint64_t kMs = 1000000ULL;

void testStartsFull() {
    TokenBucket bucket(1000.0, 2000.0);
    // 首帧应当能立刻发出：桶初始是满的，不需要先等一个控制周期。
    CHECK(bucket.tryAcquire(1500, 0));
    CHECK(bucket.availableBytes(0) == 500.0);
}

void testRejectsWhenShortAndReportsWait() {
    TokenBucket bucket(1000.0, 2000.0);  // 每秒 1000 字节
    CHECK(bucket.tryAcquire(1500, 0));   // 剩 500

    std::uint64_t waitNs = 0;
    // 同一时刻再要 600：不做补充，直接按当前 500 的余额算缺口。
    CHECK(!bucket.tryAcquire(600, 0, &waitNs));
    // 差 100 字节，按 1000 B/s 需 0.1 秒。
    CHECK(waitNs == 100 * kMs);
    // 被拒绝的请求不能扣额度。
    CHECK(bucket.availableBytes(0) == 500.0);
}

void testRefillsWithTime() {
    TokenBucket bucket(1000.0, 2000.0);
    CHECK(bucket.tryAcquire(2000, 0));       // 用光
    CHECK(!bucket.tryAcquire(1, 0));
    // 100 毫秒补 100 字节。
    CHECK(bucket.tryAcquire(100, 100 * kMs));
    CHECK(bucket.availableBytes(100 * kMs) == 0.0);
}

void testBurstCapAfterIdle() {
    TokenBucket bucket(1000.0, 2000.0);
    CHECK(bucket.tryAcquire(1000, 0));       // 剩 1000
    // 空闲 10 秒理论补 10000 字节，但突发额度封顶 2000。
    CHECK(bucket.availableBytes(10 * 1000 * kMs) == 2000.0);
    // 一次取满突发额度是允许的，第二次就不行了。
    CHECK(bucket.tryAcquire(2000, 10 * 1000 * kMs));
    CHECK(!bucket.tryAcquire(1, 10 * 1000 * kMs));
}

void testZeroRateNeverRefills() {
    TokenBucket bucket(0.0, 100.0);
    CHECK(bucket.tryAcquire(100, 0));
    std::uint64_t waitNs = 0;
    CHECK(!bucket.tryAcquire(1, 1000 * 1000 * kMs, &waitNs));
    // 速率为 0 时永远补不上，用最大值表示"别等了"。
    CHECK(waitNs == UINT64_MAX);
}

void testBackwardTimeIsHarmless() {
    // 调用方传了旧时间戳（不同时钟源混用）时不能补充、也不能倒扣。
    TokenBucket bucket(1000.0, 2000.0);
    CHECK(bucket.tryAcquire(1000, 5000 * kMs));
    const double before = bucket.availableBytes(5000 * kMs);
    CHECK(bucket.availableBytes(1000 * kMs) == before);
    CHECK(bucket.tryAcquire(1000, 1000 * kMs));
}

void testLargeRequestWaitsLonger() {
    TokenBucket bucket(100.0, 50.0);   // 慢速小桶：突发放不下大包
    bucket.tryAcquire(50, 0);          // 用光
    std::uint64_t waitNs = 0;
    CHECK(!bucket.tryAcquire(100, 0, &waitNs));
    // 差 100 字节，按 100 B/s 是 1 秒。
    CHECK(waitNs == 1000 * kMs);
}

}  // namespace

int main() {
    testStartsFull();
    testRejectsWhenShortAndReportsWait();
    testRefillsWithTime();
    testBurstCapAfterIdle();
    testZeroRateNeverRefills();
    testBackwardTimeIsHarmless();
    testLargeRequestWaitsLonger();
    return sserver::test::finish("token_bucket_test");
}
