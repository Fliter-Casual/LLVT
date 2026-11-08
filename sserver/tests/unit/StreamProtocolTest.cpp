// StreamProtocol：线格式黄金样本与往返一致的单元测试。
//
// 这里是双端唯一的字节级契约。做法是把各字段取成"小端展开后恰好连续递增"
// 的值（例如 0x0807060504030201 → 字节 01 02 03 04 05 06 07 08），于是
// 期望缓冲区就是 1,2,3,...,N。任何一个字段偏移写错一位，数组就断在那里，
// 比逐字段断言更容易定位，也顺手验证了整体长度。
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sserver/base/EncodedFrame.h"
#include "sserver/transport/protocol/StreamProtocol.h"
#include "support/Check.h"

namespace {

using namespace sserver::transport;

std::vector<std::uint8_t> contiguousBytes(std::size_t count) {
    std::vector<std::uint8_t> bytes(count, 0);
    for (std::size_t index = 0; index < count; ++index) {
        bytes[index] = static_cast<std::uint8_t>(index + 1);
    }
    return bytes;
}

bool equals(const std::vector<std::uint8_t> &actual, const std::vector<std::uint8_t> &expected,
            std::size_t *firstMismatch) {
    if (actual.size() != expected.size()) {
        *firstMismatch = 0;
        return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (actual[index] != expected[index]) {
            *firstMismatch = index;
            return false;
        }
    }
    return true;
}

// ==================== 尺寸 ====================

void testWireSizes() {
    CHECK(kMessageHeaderWireSize == 12);
    CHECK(kFrameDiagnosticMetadataWireSize == 40);
    CHECK(kUdpFrameFragmentHeaderWireSize == 84);
    CHECK(kUdpReceiverReportWireSize == 660);
    CHECK(kUdpKeepAliveAckWireSize == 16);
    CHECK(kUdpNackHeaderWireSize == 12);
    CHECK(kUdpNackItemWireSize == 12);
}

// ==================== 消息头 ====================

void testMessageHeaderGolden() {
    MessageHeader header{};
    fillMessageMagic(header.m_headId);
    header.m_messageType = static_cast<std::uint16_t>(MessageType::VideoStream);
    header.m_subType = static_cast<std::uint16_t>(sserver::base::StreamPayloadType::Video);
    header.m_payloadLength = 0x11223344U;

    std::vector<std::uint8_t> bytes(kMessageHeaderWireSize, 0);
    serializeMessageHeader(header, bytes.data());

    const std::vector<std::uint8_t> expected = {
            'C', 'C', 'T', 'D',          // 魔数
            0x01, 0x00,                  // 类型 = 1（小端）
            0x03, 0x00,                  // 子类型 = 3（Video）
            0x44, 0x33, 0x22, 0x11,      // 载荷长度（小端）
    };
    std::size_t mismatch = 0;
    CHECK(equals(bytes, expected, &mismatch));
    CHECK(mismatch == 0);
}

void testMessageHeaderMagicIsAlwaysWritten() {
    // 序列化无条件写入魔数，不依赖调用方先把 m_headId 填对。
    MessageHeader header{};
    header.m_messageType = static_cast<std::uint16_t>(MessageType::KeepAlive);
    std::vector<std::uint8_t> bytes(kMessageHeaderWireSize, 0);
    serializeMessageHeader(header, bytes.data());
    CHECK(bytes[0] == 'C' && bytes[1] == 'C' && bytes[2] == 'T' && bytes[3] == 'D');
}

void testMessageHeaderParse() {
    MessageHeader header{};
    fillMessageMagic(header.m_headId);
    header.m_messageType = static_cast<std::uint16_t>(MessageType::UdpNack);
    header.m_subType = 0;
    header.m_payloadLength = 4242;

    std::vector<std::uint8_t> bytes(kMessageHeaderWireSize, 0);
    serializeMessageHeader(header, bytes.data());

    MessageHeader parsed{};
    CHECK(parseMessageHeader(bytes.data(), bytes.size(), &parsed));
    CHECK(parsed.m_messageType == static_cast<std::uint16_t>(MessageType::UdpNack));
    CHECK(parsed.m_payloadLength == 4242);
    CHECK(hasValidMessageMagic(parsed));
}

void testMessageHeaderRejectsShortBuffer() {
    std::vector<std::uint8_t> bytes(kMessageHeaderWireSize - 1, 0);
    MessageHeader parsed{};
    // 长度不够一个头就必须失败，不能读越界后再靠魔数碰运气。
    CHECK(!parseMessageHeader(bytes.data(), bytes.size(), &parsed));
    CHECK(!parseMessageHeader(nullptr, 0, &parsed));
    CHECK(!parseMessageHeader(bytes.data(), bytes.size(), nullptr));
}

// ==================== 帧元数据 ====================

void testFrameDiagnosticMetadataGolden() {
    FrameDiagnosticMetadata metadata{};
    // 每个字段的小端展开都与下一字段首尾相接（01..08, 09..10, 11..18, ...），
    // 于是整块期望值就是 1,2,3,...,40。字段值写错一位，序列就断在该处。
    metadata.m_sequence = 0x0807060504030201ULL;
    metadata.m_captureTimestampNs = 0x100F0E0D0C0B0A09ULL;
    metadata.m_encodeStartTimestampNs = 0x1817161514131211ULL;
    metadata.m_encodeEndTimestampNs = 0x201F1E1D1C1B1A19ULL;
    metadata.m_transportSendTimestampNs = 0x2827262524232221ULL;

    std::vector<std::uint8_t> bytes(kFrameDiagnosticMetadataWireSize, 0);
    serializeFrameDiagnosticMetadata(metadata, bytes.data());

    std::size_t mismatch = 0;
    CHECK(equals(bytes, contiguousBytes(kFrameDiagnosticMetadataWireSize), &mismatch));
    CHECK(mismatch == 0);
}

void testFrameDiagnosticMetadataRoundTrip() {
    FrameDiagnosticMetadata metadata{};
    metadata.m_sequence = 9001;
    metadata.m_captureTimestampNs = 111;
    metadata.m_encodeStartTimestampNs = 222;
    metadata.m_encodeEndTimestampNs = 333;
    metadata.m_transportSendTimestampNs = 444;

    std::vector<std::uint8_t> bytes(kFrameDiagnosticMetadataWireSize, 0);
    serializeFrameDiagnosticMetadata(metadata, bytes.data());

    FrameDiagnosticMetadata parsed{};
    parseFrameDiagnosticMetadata(bytes.data(), &parsed);
    CHECK(parsed.m_sequence == 9001);
    CHECK(parsed.m_captureTimestampNs == 111);
    CHECK(parsed.m_encodeStartTimestampNs == 222);
    CHECK(parsed.m_encodeEndTimestampNs == 333);
    CHECK(parsed.m_transportSendTimestampNs == 444);
}

// ==================== 分片头 ====================

void testFragmentHeaderGolden() {
    UdpFrameFragmentHeader header{};
    // 同上的构造方式，按 8/4/2 字节字段的宽度逐段接续，整块期望 1..84。
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

    std::size_t mismatch = 0;
    CHECK(equals(bytes, contiguousBytes(kUdpFrameFragmentHeaderWireSize), &mismatch));
    CHECK(mismatch == 0);
}

void testFragmentHeaderRoundTrip() {
    UdpFrameFragmentHeader header{};
    header.m_frameSequence = 77;
    header.m_captureTimestampNs = 1000;
    header.m_encodeStartTimestampNs = 2000;
    header.m_encodeEndTimestampNs = 3000;
    header.m_transportSendTimestampNs = 4000;
    header.m_framePayloadSize = 123456;
    header.m_fragmentOffset = 2400;
    header.m_fragmentIndex = 2;
    header.m_fragmentCount = 52;
    header.m_fragmentRole = static_cast<std::uint16_t>(UdpFragmentRole::XorParity);
    header.m_parityGroupIndex = 7;
    header.m_originalPacketSequence = 555;
    header.m_packetSendTimestampNs = 6000;
    header.m_transportFrameIndex = 78;
    header.m_frameFlags = 1;  // 关键帧

    std::vector<std::uint8_t> bytes(kUdpFrameFragmentHeaderWireSize, 0);
    serializeUdpFrameFragmentHeader(header, bytes.data());

    UdpFrameFragmentHeader parsed{};
    parseUdpFrameFragmentHeader(bytes.data(), &parsed);
    CHECK(parsed.m_frameSequence == 77);
    CHECK(parsed.m_framePayloadSize == 123456);
    CHECK(parsed.m_fragmentOffset == 2400);
    CHECK(parsed.m_fragmentIndex == 2);
    CHECK(parsed.m_fragmentCount == 52);
    CHECK(parsed.m_fragmentRole == 1);
    CHECK(parsed.m_parityGroupIndex == 7);
    CHECK(parsed.m_originalPacketSequence == 555);
    CHECK(parsed.m_transportFrameIndex == 78);
    CHECK(parsed.m_frameFlags == 1);
}

// ==================== 接收报告 ====================

void testEmptyReportIsAllZero() {
    UdpReceiverReport report{};
    std::vector<std::uint8_t> bytes(kUdpReceiverReportWireSize, 0xAA);
    serializeUdpReceiverReport(report, bytes.data());
    for (std::uint8_t byte : bytes) {
        CHECK(byte == 0);
    }
}

void testReportCountAndSampleOffsets() {
    // 208 偏移是 4 字节样本数，212 起是 16 个 28 字节样本：
    // 这处"变长边界"最容易算错，单独钉死。
    UdpReceiverReport report{};
    report.m_reportTimestampNs = 0x0102030405060708ULL;
    report.m_packetFeedbackCount = 2;
    report.m_packetFeedback[0].m_sequence = 0x11;
    report.m_packetFeedback[1].m_sequence = 0x22;
    report.m_packetFeedback[1].m_wireBytes = 0xAABBCCDD;

    std::vector<std::uint8_t> bytes(kUdpReceiverReportWireSize, 0);
    serializeUdpReceiverReport(report, bytes.data());

    // 时间戳小端：08 07 06 05 04 03 02 01
    CHECK(bytes[0] == 0x08 && bytes[1] == 0x07 && bytes[7] == 0x01);
    CHECK(bytes[208] == 2 && bytes[209] == 0 && bytes[210] == 0 && bytes[211] == 0);
    CHECK(bytes[212] == 0x11);
    // 第二个样本起点 = 212 + 28 = 240；wireBytes 在样本内偏移 24。
    CHECK(bytes[240] == 0x22);
    CHECK(bytes[240 + 24] == 0xDD && bytes[240 + 25] == 0xCC);
}

void testReportRoundTrip() {
    UdpReceiverReport report{};
    report.m_reportTimestampNs = 777;
    report.m_datagramsReceived = 1000;
    report.m_invalidDatagrams = 3;
    report.m_fragmentsReceived = 9000;
    report.m_duplicateFragments = 12;
    report.m_timedOutFragments = 4;
    report.m_timedOutFrames = 1;
    report.m_completedFrames = 300;
    report.m_reorderedFrames = 9;
    report.m_jitterSamples = 201;
    report.m_jitterLastMs = 12.5;
    report.m_jitterAvgMs = 3.25;
    report.m_jitterMaxMs = 87.75;
    report.m_rttNs = 25000000;
    report.m_jitterP95Ms = 21.5;
    report.m_fecRecoveredFragments = 6;
    report.m_fecRecoveredFrames = 2;
    report.m_nackRequestsSent = 5;
    report.m_originalPacketsExpected = 5000;
    report.m_originalPacketsLost = 40;
    report.m_originalPacketsReceived = 4960;
    report.m_transportFramesExpected = 320;
    report.m_transportFramesLost = 3;
    report.m_retransmittedFragmentsReceived = 18;
    report.m_nackRecoveredFrames = 7;
    report.m_receiveRateBps = 2500000;
    report.m_packetFeedbackCount = 1;
    report.m_packetFeedback[0].m_sequence = 42;
    report.m_packetFeedback[0].m_sendTimestampNs = 100;
    report.m_packetFeedback[0].m_arrivalTimestampNs = 130;
    report.m_packetFeedback[0].m_wireBytes = 1200;

    std::vector<std::uint8_t> bytes(kUdpReceiverReportWireSize, 0);
    serializeUdpReceiverReport(report, bytes.data());

    UdpReceiverReport parsed{};
    parseUdpReceiverReport(bytes.data(), &parsed);
    CHECK(parsed.m_reportTimestampNs == 777);
    CHECK(parsed.m_datagramsReceived == 1000);
    CHECK(parsed.m_invalidDatagrams == 3);
    CHECK(parsed.m_fragmentsReceived == 9000);
    CHECK(parsed.m_duplicateFragments == 12);
    CHECK(parsed.m_timedOutFragments == 4);
    CHECK(parsed.m_timedOutFrames == 1);
    CHECK(parsed.m_completedFrames == 300);
    CHECK(parsed.m_reorderedFrames == 9);
    CHECK(parsed.m_jitterSamples == 201);
    CHECK(parsed.m_jitterLastMs == 12.5);
    CHECK(parsed.m_jitterAvgMs == 3.25);
    CHECK(parsed.m_jitterMaxMs == 87.75);
    CHECK(parsed.m_rttNs == 25000000);
    CHECK(parsed.m_jitterP95Ms == 21.5);
    CHECK(parsed.m_fecRecoveredFragments == 6);
    CHECK(parsed.m_fecRecoveredFrames == 2);
    CHECK(parsed.m_nackRequestsSent == 5);
    CHECK(parsed.m_originalPacketsExpected == 5000);
    CHECK(parsed.m_originalPacketsLost == 40);
    CHECK(parsed.m_originalPacketsReceived == 4960);
    CHECK(parsed.m_transportFramesExpected == 320);
    CHECK(parsed.m_transportFramesLost == 3);
    CHECK(parsed.m_retransmittedFragmentsReceived == 18);
    CHECK(parsed.m_nackRecoveredFrames == 7);
    CHECK(parsed.m_receiveRateBps == 2500000);
    CHECK(parsed.m_packetFeedbackCount == 1);
    CHECK(parsed.m_packetFeedback[0].m_sequence == 42);
    CHECK(parsed.m_packetFeedback[0].m_sendTimestampNs == 100);
    CHECK(parsed.m_packetFeedback[0].m_arrivalTimestampNs == 130);
    CHECK(parsed.m_packetFeedback[0].m_wireBytes == 1200);
}

// ==================== 保活确认与 NACK ====================

void testKeepAliveAckRoundTrip() {
    UdpKeepAliveAck ack{};
    ack.m_clientReportTimestampNs = 123456789012ULL;
    ack.m_serverTimestampNs = 987654321098ULL;

    std::vector<std::uint8_t> bytes(kUdpKeepAliveAckWireSize, 0);
    serializeUdpKeepAliveAck(ack, bytes.data());

    UdpKeepAliveAck parsed{};
    parseUdpKeepAliveAck(bytes.data(), &parsed);
    CHECK(parsed.m_clientReportTimestampNs == 123456789012ULL);
    CHECK(parsed.m_serverTimestampNs == 987654321098ULL);
}

void testNackRoundTrip() {
    UdpNackHeader header{};
    header.m_requestTimestampNs = 424242;
    header.m_requestCount = 3;

    std::vector<std::uint8_t> headerBytes(kUdpNackHeaderWireSize, 0);
    serializeUdpNackHeader(header, headerBytes.data());
    UdpNackHeader parsedHeader{};
    parseUdpNackHeader(headerBytes.data(), &parsedHeader);
    CHECK(parsedHeader.m_requestTimestampNs == 424242);
    CHECK(parsedHeader.m_requestCount == 3);

    UdpNackItem item{};
    item.m_frameSequence = 88;
    item.m_fragmentIndex = 17;

    std::vector<std::uint8_t> itemBytes(kUdpNackItemWireSize, 0);
    serializeUdpNackItem(item, itemBytes.data());
    UdpNackItem parsedItem{};
    parseUdpNackItem(itemBytes.data(), &parsedItem);
    CHECK(parsedItem.m_frameSequence == 88);
    CHECK(parsedItem.m_fragmentIndex == 17);
    CHECK(parsedItem.m_reserved == 0);
}

// ==================== 端序原语 ====================

void testLittleEndianPrimitives() {
    std::uint8_t buffer[8] = {0};

    writeLe16(0x1234, buffer);
    CHECK(buffer[0] == 0x34 && buffer[1] == 0x12);
    CHECK(readLe16(buffer) == 0x1234);

    writeLe32(0x89ABCDEFU, buffer);
    CHECK(buffer[0] == 0xEF && buffer[3] == 0x89);
    CHECK(readLe32(buffer) == 0x89ABCDEFU);

    writeLe64(0x0123456789ABCDEFULL, buffer);
    CHECK(buffer[0] == 0xEF && buffer[7] == 0x01);
    CHECK(readLe64(buffer) == 0x0123456789ABCDEFULL);

    // double 走位表示：能原样回来，且位模式就是 IEEE-754 的小端展开。
    CHECK(wireBitsToDouble(doubleToWireBits(12.5)) == 12.5);
    CHECK(doubleToWireBits(-0.0) == 0x8000000000000000ULL);
}

}  // namespace

int main() {
    testWireSizes();
    testMessageHeaderGolden();
    testMessageHeaderMagicIsAlwaysWritten();
    testMessageHeaderParse();
    testMessageHeaderRejectsShortBuffer();
    testFrameDiagnosticMetadataGolden();
    testFrameDiagnosticMetadataRoundTrip();
    testFragmentHeaderGolden();
    testFragmentHeaderRoundTrip();
    testEmptyReportIsAllZero();
    testReportCountAndSampleOffsets();
    testReportRoundTrip();
    testKeepAliveAckRoundTrip();
    testNackRoundTrip();
    testLittleEndianPrimitives();
    return sserver::test::finish("stream_protocol_test");
}
