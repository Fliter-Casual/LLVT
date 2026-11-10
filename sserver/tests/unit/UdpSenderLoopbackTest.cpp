// UdpSender：分片发送的环回端到端测试。
//
// 在一个进程里起收、发两个套接字：发送端把一帧切片发到 127.0.0.1，接收端
// 逐片解析头部并按偏移拼回原始载荷。这样验证的是真实的 socket 路径与真实
// 的线格式，而不是把序列化函数自己跟自己比一遍。
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sserver/base/EncodedFrame.h"
#include "sserver/transport/protocol/StreamProtocol.h"
#include "sserver/transport/udp/UdpSender.h"
#include "sserver/transport/udp/UdpSocket.h"
#include "support/Check.h"

namespace {

using namespace sserver::transport;
using sserver::base::EncodedFrame;

constexpr std::size_t kHeaderBytes = kMessageHeaderWireSize + kUdpFrameFragmentHeaderWireSize;
constexpr int kReceiveTimeoutMs = 2000;

EncodedFrame makeFrame(std::size_t payloadBytes) {
    EncodedFrame frame;
    frame.m_sequence = 42;
    frame.m_encodingId = 7;
    frame.m_captureTimestampNs = 1000;
    frame.m_encodeStartTimestampNs = 2000;
    frame.m_encodeEndTimestampNs = 3000;
    frame.m_isKeyframe = true;
    // 可预测的图案：拼回来的字节错一位都能被发现。
    frame.m_payload.resize(payloadBytes);
    for (std::size_t index = 0; index < payloadBytes; ++index) {
        frame.m_payload[index] = static_cast<std::uint8_t>((index * 7U + 3U) & 0xFFU);
    }
    return frame;
}

// 一条数据报解析后的关键字段，便于逐项断言。
struct ParsedDatagram {
    MessageHeader m_header{};
    UdpFrameFragmentHeader m_fragment{};
    std::vector<std::uint8_t> m_payload;
};

bool receiveAndParse(UdpSocket &receiver, ParsedDatagram *out, std::string *error) {
    std::vector<std::uint8_t> buffer(2048, 0);
    const int received = receiver.recvFrom(buffer.data(), buffer.size(), kReceiveTimeoutMs,
                                           nullptr, nullptr, error);
    if (received <= 0) {
        return false;
    }
    if (static_cast<std::size_t>(received) < kHeaderBytes) {
        *error = "datagram shorter than the fixed headers";
        return false;
    }
    if (!parseMessageHeader(buffer.data(), static_cast<std::size_t>(received), &out->m_header)) {
        *error = "message header parse failed";
        return false;
    }
    parseUdpFrameFragmentHeader(buffer.data() + kMessageHeaderWireSize, &out->m_fragment);
    const std::size_t payloadBytes =
            static_cast<std::size_t>(received) - kHeaderBytes;
    out->m_payload.assign(buffer.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes),
                          buffer.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + payloadBytes));
    return true;
}

void testFragmentationAndReassembly() {
    UdpSocket receiver;
    std::string error;
    CHECK(receiver.open(&error));
    CHECK(receiver.bind("127.0.0.1", 0, &error));
    const int receiverPort = receiver.boundPort();
    CHECK(receiverPort > 0);

    UdpSenderConfig config;
    config.m_fragmentPayloadBytes = 1200;  // 整形关闭：测试要快
    UdpSender sender(config);
    CHECK(sender.open("127.0.0.1", 0, &error));

    const EncodedFrame frame = makeFrame(5000);
    UdpSender::SendResult result;
    CHECK(sender.sendFrame(frame, "127.0.0.1", receiverPort, 900, 4000, &result, &error));

    // 5000 字节按 1200 切片：1200×4 + 200，共 5 片。
    CHECK(result.m_datagrams == 5);
    CHECK(result.m_bytes == 5 * kHeaderBytes + 5000);
    CHECK(!result.m_paced);

    std::vector<std::uint8_t> reassembled(5000, 0);
    std::size_t receivedDatagrams = 0;
    for (std::size_t index = 0; index < 5; ++index) {
        ParsedDatagram parsed;
        CHECK(receiveAndParse(receiver, &parsed, &error));
        ++receivedDatagrams;

        // 消息头：魔数、类型、子类型、载荷长度。
        CHECK(hasValidMessageMagic(parsed.m_header));
        CHECK(parsed.m_header.m_messageType
              == static_cast<std::uint16_t>(MessageType::VideoStream));
        CHECK(parsed.m_header.m_subType
              == static_cast<std::uint16_t>(sserver::base::StreamPayloadType::Video));
        CHECK(parsed.m_header.m_payloadLength
              == static_cast<std::uint32_t>(kUdpFrameFragmentHeaderWireSize + parsed.m_payload.size()));

        // 分片头：一帧内共享的字段。
        CHECK(parsed.m_fragment.m_frameSequence == 42);
        CHECK(parsed.m_fragment.m_captureTimestampNs == 1000);
        CHECK(parsed.m_fragment.m_encodeStartTimestampNs == 2000);
        CHECK(parsed.m_fragment.m_encodeEndTimestampNs == 3000);
        CHECK(parsed.m_fragment.m_transportSendTimestampNs == 4000);
        CHECK(parsed.m_fragment.m_transportFrameIndex == 900);
        CHECK(parsed.m_fragment.m_framePayloadSize == 5000);
        CHECK(parsed.m_fragment.m_fragmentCount == 5);
        CHECK(parsed.m_fragment.m_frameFlags == 1);  // 关键帧
        CHECK(parsed.m_fragment.m_fragmentRole
              == static_cast<std::uint16_t>(UdpFragmentRole::Data));

        // 逐片变化的字段。
        CHECK(parsed.m_fragment.m_fragmentIndex == static_cast<std::uint16_t>(index));
        CHECK(parsed.m_fragment.m_fragmentOffset == static_cast<std::uint32_t>(index * 1200));
        CHECK(parsed.m_fragment.m_originalPacketSequence == index);
        CHECK(parsed.m_payload.size() == (index == 4 ? 200U : 1200U));

        // 按偏移拼回原帧：偏移写错会直接表现为内容不符。
        const std::size_t offset = parsed.m_fragment.m_fragmentOffset;
        for (std::size_t byte = 0; byte < parsed.m_payload.size(); ++byte) {
            reassembled[offset + byte] = parsed.m_payload[byte];
        }
    }
    CHECK(receivedDatagrams == 5);
    CHECK(reassembled == frame.m_payload);

    sender.close();
    receiver.close();
}

void testSingleFragmentFrame() {
    UdpSocket receiver;
    std::string error;
    CHECK(receiver.open(&error));
    CHECK(receiver.bind("127.0.0.1", 0, &error));

    UdpSender sender;
    CHECK(sender.open("127.0.0.1", 0, &error));

    const EncodedFrame frame = makeFrame(500);
    UdpSender::SendResult result;
    CHECK(sender.sendFrame(frame, "127.0.0.1", receiver.boundPort(), 1, 5000, &result, &error));
    CHECK(result.m_datagrams == 1);

    ParsedDatagram parsed;
    CHECK(receiveAndParse(receiver, &parsed, &error));
    CHECK(parsed.m_fragment.m_fragmentCount == 1);
    CHECK(parsed.m_fragment.m_fragmentIndex == 0);
    CHECK(parsed.m_payload == frame.m_payload);

    sender.close();
    receiver.close();
}

void testRejectsEmptyPayload() {
    UdpSender sender;
    std::string error;
    CHECK(sender.open("127.0.0.1", 0, &error));

    EncodedFrame frame;
    frame.m_payload.clear();
    UdpSender::SendResult result;
    CHECK(!sender.sendFrame(frame, "127.0.0.1", 9, 0, 0, &result, &error));
    CHECK(error.find("empty frame payload") != std::string::npos);
}

void testPacketSequenceKeepsIncreasingAcrossFrames() {
    UdpSocket receiver;
    std::string error;
    CHECK(receiver.open(&error));
    CHECK(receiver.bind("127.0.0.1", 0, &error));

    UdpSender sender;
    CHECK(sender.open("127.0.0.1", 0, &error));
    const int port = receiver.boundPort();

    UdpSender::SendResult result;
    CHECK(sender.sendFrame(makeFrame(300), "127.0.0.1", port, 1, 100, &result, &error));
    CHECK(sender.sendFrame(makeFrame(300), "127.0.0.1", port, 2, 200, &result, &error));

    // 首发序号跨帧连续递增：接收端用它结算首发丢包，跨帧回绕会让统计错乱。
    ParsedDatagram first;
    ParsedDatagram second;
    CHECK(receiveAndParse(receiver, &first, &error));
    CHECK(receiveAndParse(receiver, &second, &error));
    CHECK(first.m_fragment.m_originalPacketSequence == 0);
    CHECK(second.m_fragment.m_originalPacketSequence == 1);
    CHECK(second.m_fragment.m_transportFrameIndex == 2);

    sender.close();
    receiver.close();
}

}  // namespace

int main() {
    testFragmentationAndReassembly();
    testSingleFragmentFrame();
    testRejectsEmptyPayload();
    testPacketSequenceKeepsIncreasingAcrossFrames();
    return sserver::test::finish("udp_sender_loopback_test");
}
