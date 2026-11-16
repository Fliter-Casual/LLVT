// StreamProtocol（客户端镜像）：线格式一致性的黄金样本测试。
//
// 服务端与客户端是两个独立工程，各自持有一份线格式定义。这里用与服务端
// StreamProtocolTest 完全相同的取值与断言：只要两边都通过，字节布局就一致；
// 任何一侧改字段顺序，这边的序列号断点会立刻指出来。
#include <cstddef>
#include <cstdint>
#include <vector>

#include "sclient/transport/protocol/StreamProtocol.h"
#include "support/Check.h"

namespace {

using namespace sclient::transport;

std::vector<std::uint8_t> contiguousBytes(std::size_t count) {
    std::vector<std::uint8_t> bytes(count, 0);
    for (std::size_t index = 0; index < count; ++index) {
        bytes[index] = static_cast<std::uint8_t>(index + 1);
    }
    return bytes;
}

void testWireSizes() {
    CHECK(kMessageHeaderWireSize == 12);
    CHECK(kFrameDiagnosticMetadataWireSize == 40);
    CHECK(kUdpFrameFragmentHeaderWireSize == 84);
    CHECK(kUdpReceiverReportWireSize == 660);
    CHECK(kUdpKeepAliveAckWireSize == 16);
    CHECK(kUdpNackHeaderWireSize == 12);
    CHECK(kUdpNackItemWireSize == 12);
}

void testMessageHeaderGolden() {
    MessageHeader header{};
    fillMessageMagic(header.m_headId);
    header.m_messageType = static_cast<std::uint16_t>(MessageType::VideoStream);
    header.m_subType = 3;
    header.m_payloadLength = 0x11223344U;

    std::vector<std::uint8_t> bytes(kMessageHeaderWireSize, 0);
    serializeMessageHeader(header, bytes.data());

    const std::vector<std::uint8_t> expected = {
            'C', 'C', 'T', 'D',
            0x01, 0x00,
            0x03, 0x00,
            0x44, 0x33, 0x22, 0x11,
    };
    CHECK(bytes == expected);
}

void testFragmentHeaderGolden() {
    UdpFrameFragmentHeader header{};
    header.m_frameSequence = 0x0807060504030201ULL;
    header.m_captureTimestampNs = 0x100F0E0D0C0B0A09ULL;
    header.m_encodeStartTimestampNs = 0x1817161514131211ULL;
    header.m_encodeEndTimestampNs = 0x201F1E1D1C1B1A19ULL;
    header.m_transportSendTimestampNs = 0x2827262524232221ULL;
    header.m_framePayloadSize = 0x2C2B2A29U;
    header.m_fragmentOffset = 0x302F2E2DU;
    header.m_fragmentIndex = 0x3231;
    header.m_fragmentCount = 0x3433;
    header.m_fragmentRole = 0x3635;
    header.m_parityGroupIndex = 0x3837;
    header.m_originalPacketSequence = 0x403F3E3D3C3B3A39ULL;
    header.m_packetSendTimestampNs = 0x4847464544434241ULL;
    header.m_transportFrameIndex = 0x504F4E4D4C4B4A49ULL;
    header.m_frameFlags = 0x54535251U;

    std::vector<std::uint8_t> bytes(kUdpFrameFragmentHeaderWireSize, 0);
    serializeUdpFrameFragmentHeader(header, bytes.data());

    // 每段小端展开后逐段接续，期望值就是 1..84：偏移错一位索引就会断。
    CHECK(bytes == contiguousBytes(kUdpFrameFragmentHeaderWireSize));
}

void testFragmentHeaderRoundTrip() {
    UdpFrameFragmentHeader header{};
    header.m_frameSequence = 77;
    header.m_captureTimestampNs = 1000;
    header.m_encodeStartTimestampNs = 2000;
    header.m_encodeEndTimestampNs = 3000;
    header.m_transportSendTimestampNs = 4000;
    header.m_framePayloadSize = 1500;
    header.m_fragmentOffset = 1200;
    header.m_fragmentIndex = 1;
    header.m_fragmentCount = 2;
    header.m_fragmentRole = static_cast<std::uint16_t>(UdpFragmentRole::Data);
    header.m_parityGroupIndex = 0;
    header.m_originalPacketSequence = 9;
    header.m_packetSendTimestampNs = 4100;
    header.m_transportFrameIndex = 78;
    header.m_frameFlags = 1;

    std::vector<std::uint8_t> bytes(kUdpFrameFragmentHeaderWireSize, 0);
    serializeUdpFrameFragmentHeader(header, bytes.data());
    UdpFrameFragmentHeader parsed{};
    parseUdpFrameFragmentHeader(bytes.data(), &parsed);

    CHECK(parsed.m_frameSequence == 77);
    CHECK(parsed.m_framePayloadSize == 1500);
    CHECK(parsed.m_fragmentOffset == 1200);
    CHECK(parsed.m_fragmentIndex == 1);
    CHECK(parsed.m_fragmentCount == 2);
    CHECK(parsed.m_frameFlags == 1);
}

void testReportOffsets() {
    UdpReceiverReport report{};
    report.m_packetFeedbackCount = 2;
    report.m_packetFeedback[1].m_sequence = 0x22;
    std::vector<std::uint8_t> bytes(kUdpReceiverReportWireSize, 0);
    serializeUdpReceiverReport(report, bytes.data());
    // 变长边界的三个关键偏移必须与服务端一致。
    CHECK(bytes[208] == 2);
    CHECK(bytes[212] == 0);
    CHECK(bytes[240] == 0x22);
}

void testShortBufferRejected() {
    std::vector<std::uint8_t> bytes(kMessageHeaderWireSize - 1, 0);
    MessageHeader parsed{};
    CHECK(!parseMessageHeader(bytes.data(), bytes.size(), &parsed));
}

}  // namespace

int main() {
    testWireSizes();
    testMessageHeaderGolden();
    testFragmentHeaderGolden();
    testFragmentHeaderRoundTrip();
    testReportOffsets();
    testShortBufferRejected();
    return sclient::test::finish("stream_protocol_mirror_test");
}
