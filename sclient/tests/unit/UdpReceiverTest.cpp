// UdpReceiver：真实套接字上的收帧测试。
//
// 用镜像序列化器构造数据报、经 127.0.0.1 发给接收端，验证"校验 → 解析 →
// 组装"整条路径；同时确认非视频消息与垃圾数据会被跳过而不是打断收帧。
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sclient/transport/protocol/StreamProtocol.h"
#include "sclient/transport/udp/UdpReceiver.h"
#include "sclient/transport/udp/UdpSocket.h"
#include "support/Check.h"

namespace {

using namespace sclient::transport;

std::vector<std::uint8_t> makeDatagram(const UdpFrameFragmentHeader &fragment,
                                       const std::uint8_t *payload,
                                       std::size_t payloadBytes) {
    std::vector<std::uint8_t> datagram(
            kMessageHeaderWireSize + kUdpFrameFragmentHeaderWireSize + payloadBytes, 0);
    MessageHeader header{};
    fillMessageMagic(header.m_headId);
    header.m_messageType = static_cast<std::uint16_t>(MessageType::VideoStream);
    header.m_subType = 3;
    header.m_payloadLength =
            static_cast<std::uint32_t>(kUdpFrameFragmentHeaderWireSize + payloadBytes);
    serializeMessageHeader(header, datagram.data());
    serializeUdpFrameFragmentHeader(fragment, datagram.data() + kMessageHeaderWireSize);
    std::memcpy(datagram.data() + kMessageHeaderWireSize + kUdpFrameFragmentHeaderWireSize,
                payload, payloadBytes);
    return datagram;
}

UdpFrameFragmentHeader makeFragment(std::uint64_t sequence, std::uint32_t frameBytes,
                                    std::uint32_t offset, std::uint16_t index,
                                    std::uint16_t count, bool keyframe) {
    UdpFrameFragmentHeader fragment{};
    fragment.m_frameSequence = sequence;
    fragment.m_captureTimestampNs = 111;
    fragment.m_encodeStartTimestampNs = 222;
    fragment.m_encodeEndTimestampNs = 333;
    fragment.m_transportSendTimestampNs = 444;
    fragment.m_framePayloadSize = frameBytes;
    fragment.m_fragmentOffset = offset;
    fragment.m_fragmentIndex = index;
    fragment.m_fragmentCount = count;
    fragment.m_fragmentRole = static_cast<std::uint16_t>(UdpFragmentRole::Data);
    fragment.m_frameFlags = keyframe ? 1U : 0U;
    return fragment;
}

void testReceivesAndAssemblesFrame() {
    UdpReceiver receiver;
    std::string error;
    CHECK(receiver.open("127.0.0.1", 0, &error));

    UdpSocket sender;
    CHECK(sender.open(&error));

    std::vector<std::uint8_t> frameData(1500, 0);
    for (std::size_t index = 0; index < frameData.size(); ++index) {
        frameData[index] = static_cast<std::uint8_t>((index * 3U) & 0xFFU);
    }

    // 先发一条保活消息和一段垃圾：接收端必须跳过它们继续等视频帧。
    {
        std::vector<std::uint8_t> keepAlive(kMessageHeaderWireSize + kUdpReceiverReportWireSize, 0);
        MessageHeader header{};
        fillMessageMagic(header.m_headId);
        header.m_messageType = static_cast<std::uint16_t>(MessageType::KeepAlive);
        header.m_payloadLength = kUdpReceiverReportWireSize;
        serializeMessageHeader(header, keepAlive.data());
        CHECK(sender.sendTo("127.0.0.1", receiver.boundPort(), keepAlive.data(), keepAlive.size(),
                            &error));
        const std::vector<std::uint8_t> garbage = {1, 2, 3, 4, 5};
        CHECK(sender.sendTo("127.0.0.1", receiver.boundPort(), garbage.data(), garbage.size(),
                            &error));
    }

    // 故意乱序：先发第二片，再发第一片。
    const auto second = makeDatagram(
            makeFragment(7, 1500, 1200, 1, 2, true), frameData.data() + 1200, 300);
    CHECK(sender.sendTo("127.0.0.1", receiver.boundPort(), second.data(), second.size(), &error));

    ReceivedFrame frame;
    CHECK(!receiver.receiveFrame(&frame, 50, &error));   // 只有一片时收不到整帧

    const auto first = makeDatagram(
            makeFragment(7, 1500, 0, 0, 2, true), frameData.data(), 1200);
    CHECK(sender.sendTo("127.0.0.1", receiver.boundPort(), first.data(), first.size(), &error));

    CHECK(receiver.receiveFrame(&frame, 2000, &error));
    CHECK(frame.m_payload == frameData);
    CHECK(frame.m_metadata.m_frameSequence == 7);
    CHECK(frame.m_metadata.m_captureTimestampNs == 111);
    CHECK(frame.m_metadata.m_frameFlags == 1);
    CHECK(frame.m_receiveTimestampNs > 0);

    receiver.close();
    sender.close();
}

void testTimesOutWithoutData() {
    UdpReceiver receiver;
    std::string error;
    CHECK(receiver.open("127.0.0.1", 0, &error));
    ReceivedFrame frame;
    // 超时是正常结果，不是错误：调用方靠它回到"还能干别的"的循环里。
    CHECK(!receiver.receiveFrame(&frame, 50, &error));
    receiver.close();
}

void testInvalidFragmentDoesNotCompleteFrame() {
    UdpReceiver receiver;
    std::string error;
    CHECK(receiver.open("127.0.0.1", 0, &error));

    UdpSocket sender;
    CHECK(sender.open(&error));

    // 分片序号越界：必须被拒，且不能留下半截记录。
    std::vector<std::uint8_t> payload(100, 0);
    const auto bad = makeDatagram(makeFragment(9, 100, 0, 5, 2, false), payload.data(), 50);
    CHECK(sender.sendTo("127.0.0.1", receiver.boundPort(), bad.data(), bad.size(), &error));

    ReceivedFrame frame;
    CHECK(!receiver.receiveFrame(&frame, 100, &error));
    CHECK(receiver.stats().m_invalidFragments >= 1);

    receiver.close();
    sender.close();
}

}  // namespace

int main() {
    testReceivesAndAssemblesFrame();
    testTimesOutWithoutData();
    testInvalidFragmentDoesNotCompleteFrame();
    return sclient::test::finish("udp_receiver_test");
}
