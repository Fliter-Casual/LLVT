// ThreadSafeQueue：入队策略、丢弃计数与限时阻塞弹出的单元测试。
//
// 重点验证「丢帧可数」「容量为 0 只保留新值」「关键帧优先被保护」三条语义，
// 以及阻塞等待在超时和生产者到达两种情形下的行为。
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "sserver/base/ThreadSafeQueue.h"
#include "support/Check.h"

namespace {

struct TestFrame {
    int m_sequence;
    bool m_isKeyframe;
};

using sserver::base::ThreadSafeQueue;

void testFifoBasics() {
    ThreadSafeQueue<int> queue;
    CHECK(queue.empty());
    CHECK(queue.size() == 0);

    queue.push(1);
    queue.push(2);
    queue.push(3);
    CHECK(queue.size() == 3);

    int value = 0;
    CHECK(queue.tryPop(&value));
    CHECK(value == 1);
    CHECK(queue.tryPop(&value));
    CHECK(value == 2);
    CHECK(queue.anyMatching([](int item) { return item == 3; }));
    CHECK(!queue.anyMatching([](int item) { return item == 1; }));

    CHECK(queue.tryPop(&value));
    CHECK(value == 3);
    CHECK(!queue.tryPop(&value));

    queue.push(9);
    queue.clear();
    CHECK(queue.empty());
}

void testDropOldest() {
    ThreadSafeQueue<int> queue;
    queue.push(1);
    queue.push(2);
    queue.push(3);

    // 容量 2：入队 4 之前先淘汰最旧的 1。
    const std::size_t dropped = queue.pushDropOldestCountDropped(4, 2);
    CHECK(dropped == 2);
    CHECK(queue.size() == 2);

    int value = 0;
    CHECK(queue.tryPop(&value));
    CHECK(value == 3);
    CHECK(queue.tryPop(&value));
    CHECK(value == 4);
}

void testDropOldestZeroCapacity() {
    ThreadSafeQueue<int> queue;
    queue.push(1);
    queue.push(2);

    // 容量 0 表示只保留新值，既有元素全部丢弃，且不能死循环。
    const std::size_t dropped = queue.pushDropOldestCountDropped(7, 0);
    CHECK(dropped == 2);
    CHECK(queue.size() == 1);

    int value = 0;
    CHECK(queue.tryPop(&value));
    CHECK(value == 7);
}

void testSelectiveDropProtectsKeyframe() {
    ThreadSafeQueue<TestFrame> queue;
    queue.push(TestFrame{1, true});   // 关键帧，应被保护
    queue.push(TestFrame{2, false});
    queue.push(TestFrame{3, false});

    // 容量 2 且优先丢非关键帧：丢弃 seq=2，关键帧保留。
    const std::size_t dropped = queue.pushDropSelective(
            TestFrame{4, false}, 2,
            [](const TestFrame &frame) { return !frame.m_isKeyframe; });
    CHECK(dropped == 2);

    std::vector<int> sequences;
    TestFrame frame{0, false};
    while (queue.tryPop(&frame)) {
        sequences.push_back(frame.m_sequence);
    }
    CHECK(sequences.size() == 2);
    CHECK(sequences[0] == 1);      // 关键帧还在
    CHECK(sequences[1] == 4);
}

void testSelectiveDropFallsBackToOldest() {
    ThreadSafeQueue<TestFrame> queue;
    queue.push(TestFrame{1, true});
    queue.push(TestFrame{2, true});

    // 队列里没有可丢弃的非关键帧，退回丢最旧，仍要为新帧腾位置。
    const std::size_t dropped = queue.pushDropSelective(
            TestFrame{3, false}, 2,
            [](const TestFrame &frame) { return !frame.m_isKeyframe; });
    CHECK(dropped == 1);

    TestFrame frame{0, false};
    CHECK(queue.tryPop(&frame));
    CHECK(frame.m_sequence == 2);
    CHECK(queue.tryPop(&frame));
    CHECK(frame.m_sequence == 3);
}

void testWaitPopTimeout() {
    ThreadSafeQueue<std::string> queue;
    std::string value;

    const auto start = std::chrono::steady_clock::now();
    CHECK(!queue.waitPopFor(&value, std::chrono::milliseconds(20)));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // 超时必须是真等待，不能立即返回；给调度留足余量以免误报。
    CHECK(elapsed >= std::chrono::milliseconds(15));
    CHECK(elapsed < std::chrono::milliseconds(500));
}

void testWaitPopWakesOnPush() {
    ThreadSafeQueue<int> queue;
    int value = 0;

    std::thread producer([&queue]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        queue.push(42);
    });

    CHECK(queue.waitPopFor(&value, std::chrono::milliseconds(1000)));
    CHECK(value == 42);
    producer.join();
}

void testNotifyAllDoesNotSatisfyPredicate() {
    ThreadSafeQueue<int> queue;
    int value = 0;
    std::thread waiter([&queue, &value]() {
        // 广播唤醒后队列仍为空，等待方继续睡到超时——唤醒不等于有数据。
        CHECK(!queue.waitPopFor(&value, std::chrono::milliseconds(30)));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    queue.notifyAll();
    waiter.join();
}

}  // namespace

int main() {
    testFifoBasics();
    testDropOldest();
    testDropOldestZeroCapacity();
    testSelectiveDropProtectsKeyframe();
    testSelectiveDropFallsBackToOldest();
    testWaitPopTimeout();
    testWaitPopWakesOnPush();
    testNotifyAllDoesNotSatisfyPredicate();
    return sserver::test::finish("thread_safe_queue_test");
}
