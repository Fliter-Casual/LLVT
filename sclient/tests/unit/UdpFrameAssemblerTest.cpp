// UdpFrameAssembler：乱序、重复、缺片、畸形与超时剪枝的单元测试。
//
// 组装器是纯逻辑（时间由调用方传入），所以这些情形都能精确构造，不依赖
// 真实网络——真实网络里最难复现的恰恰就是"乱序 + 重复 + 缺一片"。
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sclient/transport/udp/UdpFrameAssembler.h"
#include "support/Check.h"

namespace {

using sclient::transport::ReceivedFrame;
using sclient::transport::UdpFragmentRole;
using sclient::transport::UdpFrameAssembler;
using sclient::transport::UdpFrameFragmentHeader;

constexpr std::uint64_t kMs = 1000000ULL;

std::vector<std::uint8_t> pattern(std::size_t bytes, std::uint8_t seed) {
    std::vector<std::uint8_t> data(bytes, 0);
    for (std::size_t index = 0; index < bytes; ++index) {
        data[index] = static_cast<std::uint8_t>(seed + index);
    }
    return data;
}

UdpFrameFragmentHeader makeFragment(std::uint64_t sequence, std::uint32_t frameBytes,
                                    std::uint32_t offset, std::uint16_t index,
                                    std::uint16_t count) {
    UdpFrameFragmentHeader fragment{};
    fragment.m_frameSequence = sequence;
    fragment.m_captureTimestampNs = 100;
    fragment.m_encodeStartTimestampNs = 200;
    fragment.m_encodeEndTimestampNs = 300;
    fragment.m_transportSendTimestampNs = 400;
    fragment.m_framePayloadSize = frameBytes;
    fragment.m_fragmentOffset = offset;
    fragment.m_fragmentIndex = index;
    fragment.m_fragmentCount = count;
    fragment.m_fragmentRole = static_cast<std::uint16_t>(UdpFragmentRole::Data);
    fragment.m_transportFrameIndex = sequence;
    return fragment;
}

// 把一帧切成两片喂进去，返回是否在两片之后凑齐。
bool feedTwoFragmentFrame(UdpFrameAssembler *assembler, const std::vector<std::uint8_t> &frameData,
                          ReceivedFrame *out, bool reverseOrder, std::uint64_t nowNs) {
    const std::size_t split = frameData.size() / 2;
    if (!reverseOrder) {
        if (assembler->addFragment(makeFragment(1, static_cast<std::uint32_t>(frameData.size()),
                                                0, 0, 2), frameData.data(), split, nowNs, out)) {
            return true;
        }
        return assembler->addFragment(makeFragment(1, static_cast<std::uint32_t>(frameData.size()),
                                                   static_cast<std::uint32_t>(split), 1, 2),
                                      frameData.data() + split, frameData.size() - split,
                                      nowNs, out);
    }
    if (assembler->addFragment(makeFragment(1, static_cast<std::uint32_t>(frameData.size()),
                                            static_cast<std::uint32_t>(split), 1, 2),
                               frameData.data() + split, frameData.size() - split, nowNs, out)) {
        return true;
    }
    return assembler->addFragment(makeFragment(1, static_cast<std::uint32_t>(frameData.size()),
                                               0, 0, 2),
                                  frameData.data(), split, nowNs, out);
}

void testInOrderCompletes() {
    UdpFrameAssembler assembler;
    const std::vector<std::uint8_t> data = pattern(1000, 7);
    ReceivedFrame frame;
    CHECK(feedTwoFragmentFrame(&assembler, data, &frame, false, 0));
    CHECK(frame.m_payload == data);
    CHECK(frame.m_metadata.m_frameSequence == 1);
    CHECK(frame.m_metadata.m_captureTimestampNs == 100);
    CHECK(frame.m_receiveTimestampNs == 0);
    CHECK(assembler.stats().m_completedFrames == 1);
    CHECK(assembler.inFlightFrames() == 0);
}

void testOutOfOrderCompletes() {
    // 乱序到达不该影响结果：位图判定完整性，与到达顺序无关。
    UdpFrameAssembler assembler;
    const std::vector<std::uint8_t> data = pattern(1000, 11);
    ReceivedFrame frame;
    CHECK(feedTwoFragmentFrame(&assembler, data, &frame, true, 5000));
    CHECK(frame.m_payload == data);
    CHECK(frame.m_receiveTimestampNs == 5000);
}

void testDuplicateFragmentCounted() {
    UdpFrameAssembler assembler;
    const std::vector<std::uint8_t> data = pattern(1000, 3);
    ReceivedFrame frame;
    const std::size_t split = data.size() / 2;
    CHECK(!assembler.addFragment(makeFragment(1, 1000, 0, 0, 2), data.data(), split, 0, &frame));
    // 重复的第一片：计数但不重复写入。
    CHECK(!assembler.addFragment(makeFragment(1, 1000, 0, 0, 2), data.data(), split, 0, &frame));
    CHECK(assembler.stats().m_duplicateFragments == 1);
    CHECK(assembler.addFragment(makeFragment(1, 1000, static_cast<std::uint32_t>(split), 1, 2),
                                data.data() + split, data.size() - split, 0, &frame));
    CHECK(frame.m_payload == data);
}

void testMissingFragmentTimesOut() {
    UdpFrameAssembler::Options options;
    options.m_assemblyTimeoutNs = 250 * kMs;
    UdpFrameAssembler assembler(options);
    const std::vector<std::uint8_t> data = pattern(1000, 5);
    ReceivedFrame frame;
    CHECK(!assembler.addFragment(makeFragment(1, 1000, 0, 0, 2), data.data(), 500, 0, &frame));
    CHECK(assembler.inFlightFrames() == 1);

    // 未到超时不剪枝。
    assembler.prune(200 * kMs);
    CHECK(assembler.inFlightFrames() == 1);

    // 超过超时即回收：半截帧没有任何用处，留着只会占内存。
    assembler.prune(300 * kMs);
    CHECK(assembler.inFlightFrames() == 0);
    CHECK(assembler.stats().m_timedOutFrames == 1);
}

void testRejectsMalformedFragments() {
    UdpFrameAssembler assembler;
    ReceivedFrame frame;
    const std::vector<std::uint8_t> data = pattern(100, 1);

    // 分片数 0
    CHECK(!assembler.addFragment(makeFragment(1, 100, 0, 0, 0), data.data(), 50, 0, &frame));
    // 序号不小于分片数
    CHECK(!assembler.addFragment(makeFragment(1, 100, 0, 2, 2), data.data(), 50, 0, &frame));
    // 偏移 + 长度越过整帧大小
    CHECK(!assembler.addFragment(makeFragment(1, 100, 80, 0, 2), data.data(), 50, 0, &frame));
    // 空载荷
    CHECK(!assembler.addFragment(makeFragment(1, 100, 0, 0, 2), data.data(), 0, 0, &frame));
    // 整帧大小 0
    CHECK(!assembler.addFragment(makeFragment(1, 0, 0, 0, 2), data.data(), 50, 0, &frame));
    // 超出单帧上限（默认 4 MiB）
    CHECK(!assembler.addFragment(makeFragment(1, 8U * 1024U * 1024U, 0, 0, 2),
                                 data.data(), 50, 0, &frame));
    // 角色非法
    UdpFrameFragmentHeader badRole = makeFragment(1, 100, 0, 0, 2);
    badRole.m_fragmentRole = 9;
    CHECK(!assembler.addFragment(badRole, data.data(), 50, 0, &frame));

    CHECK(assembler.stats().m_invalidFragments == 7);
    // 畸形分片不能留下在途记录。
    CHECK(assembler.inFlightFrames() == 0);
}

void testInterleavedFramesAssembleSeparately() {
    // 两帧交错到达：记录按帧号隔离，互不干扰。
    UdpFrameAssembler assembler;
    const std::vector<std::uint8_t> first = pattern(400, 10);
    const std::vector<std::uint8_t> second = pattern(400, 90);
    ReceivedFrame frame;

    CHECK(!assembler.addFragment(makeFragment(1, 400, 0, 0, 2), first.data(), 200, 0, &frame));
    CHECK(!assembler.addFragment(makeFragment(2, 400, 0, 0, 2), second.data(), 200, 0, &frame));

    // 帧 1 的补齐片先到：交付帧 1，帧 2 仍在途。
    CHECK(assembler.addFragment(makeFragment(1, 400, 200, 1, 2), first.data() + 200, 200, 0, &frame));
    CHECK(frame.m_metadata.m_frameSequence == 1);
    CHECK(frame.m_payload == first);
    CHECK(assembler.inFlightFrames() == 1);

    // 帧 2 的补齐片随后到：交付帧 2。
    CHECK(assembler.addFragment(makeFragment(2, 400, 200, 1, 2), second.data() + 200, 200, 0, &frame));
    CHECK(frame.m_metadata.m_frameSequence == 2);
    CHECK(frame.m_payload == second);
    CHECK(assembler.stats().m_completedFrames == 2);
    CHECK(assembler.inFlightFrames() == 0);
}

void testInFlightLimitRejectsNewFrames() {
    UdpFrameAssembler::Options options;
    options.m_maxInFlightFrames = 2;
    options.m_assemblyTimeoutNs = 1000 * kMs;
    UdpFrameAssembler assembler(options);
    const std::vector<std::uint8_t> data = pattern(100, 1);
    ReceivedFrame frame;

    CHECK(!assembler.addFragment(makeFragment(1, 100, 0, 0, 2), data.data(), 50, 0, &frame));
    CHECK(!assembler.addFragment(makeFragment(2, 100, 0, 0, 2), data.data(), 50, 0, &frame));
    // 第三帧且没有超时记录可剪：拒绝，避免记录表无上限增长。
    CHECK(!assembler.addFragment(makeFragment(3, 100, 0, 0, 2), data.data(), 50, 0, &frame));
    CHECK(assembler.inFlightFrames() == 2);
}

}  // namespace

int main() {
    testInOrderCompletes();
    testOutOfOrderCompletes();
    testDuplicateFragmentCounted();
    testMissingFragmentTimesOut();
    testRejectsMalformedFragments();
    testInterleavedFramesAssembleSeparately();
    testInFlightLimitRejectsNewFrames();
    return sclient::test::finish("udp_frame_assembler_test");
}
