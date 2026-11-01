// MonotonicClock：单调时钟时间源的单元测试。
//
// 只验证「单调不减」和「跨 sleep 的差值可信」这两个能用黑盒断言的属性；
// 绝对值的纪元是实现定义的（可能是开机以来的时间），不能断言具体数值。
#include <chrono>
#include <cstdint>
#include <thread>

#include "sserver/base/MonotonicClock.h"
#include "support/Check.h"

namespace {

void testNonDecreasing() {
    const std::uint64_t first = sserver::base::monotonicNowNs();
    const std::uint64_t second = sserver::base::monotonicNowNs();
    CHECK(second >= first);
    // 防止误接一个返回 0 的占位实现。
    CHECK(first > 0);
}

void testMeasuresElapsedInterval() {
    const std::uint64_t start = sserver::base::monotonicNowNs();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::uint64_t end = sserver::base::monotonicNowNs();

    const std::uint64_t elapsedNs = end - start;
    // sleep_for 保证至少睡那么久，上界只用来兜住异常值；
    // 下界留 5ms 余量给计时器精度较差的平台。
    CHECK(elapsedNs >= 15000000ULL);
    CHECK(elapsedNs < 2000000000ULL);
}

void testNanosecondToMillisecondConversion() {
    // 全链路约定：打点是纳秒，统计和日志是毫秒，换算粒度 1e6 纳秒。
    // 这里用 /1e6 的结果与 Duration 换算交叉验证一次。
    const std::uint64_t start = sserver::base::monotonicNowNs();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const std::uint64_t elapsedNs = sserver::base::monotonicNowNs() - start;

    const auto asDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::nanoseconds(elapsedNs));
    CHECK(asDuration.count() == static_cast<long long>(elapsedNs / 1000000ULL));
    CHECK(asDuration.count() >= 5);
}

}  // namespace

int main() {
    testNonDecreasing();
    testMeasuresElapsedInterval();
    testNanosecondToMillisecondConversion();
    return sserver::test::finish("monotonic_clock_test");
}
