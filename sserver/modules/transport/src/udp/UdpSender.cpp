// ============================================================
// 文件：UdpSender.cpp
// 模块：sserver::transport
// 功能：分片与线格式拼装：每片 = 12 字节消息头 + 84 字节分片头 + 码流数据；
//       可选按令牌桶整形。发送失败立即中止本帧，不做部分重试。
// 知识点：按偏移切片、逐片头字段、整形等待与截止时刻、错误短路
// ============================================================

#include "sserver/transport/udp/UdpSender.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "sserver/transport/protocol/StreamProtocol.h"

namespace sserver {
namespace transport {

UdpSender::UdpSender(const UdpSenderConfig &config)
        : m_config(config),
          m_bucket(config.m_pacingBytesPerSecond, config.m_pacingBurstBytes) {
}

bool UdpSender::open(const std::string &bindAddress, int port, std::string *errorMessage) {
    return m_socket.open(errorMessage) && m_socket.bind(bindAddress, port, errorMessage);
}

bool UdpSender::sendFrame(const base::EncodedFrame &frame,
                          const std::string &targetAddress,
                          int targetPort,
                          std::uint64_t transportFrameIndex,
                          std::uint64_t sendTimestampNs,
                          SendResult *result,
                          std::string *errorMessage) {
    if (frame.m_payload.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "refusing to send an empty frame payload";
        }
        return false;
    }
    if (m_config.m_fragmentPayloadBytes == 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "fragment payload size must be greater than zero";
        }
        return false;
    }
    if (!m_socket.isOpen()) {
        if (errorMessage != nullptr) {
            *errorMessage = "sender socket is not open";
        }
        return false;
    }

    const std::size_t payloadBytes = frame.m_payload.size();
    const std::size_t fragmentSize = m_config.m_fragmentPayloadBytes;
    const std::size_t fragmentCount = (payloadBytes + fragmentSize - 1) / fragmentSize;

    constexpr std::size_t kHeaderBytes = kMessageHeaderWireSize + kUdpFrameFragmentHeaderWireSize;
    std::vector<std::uint8_t> datagram(kHeaderBytes + fragmentSize, 0);

    const bool pacingEnabled = m_config.m_pacingBytesPerSecond > 0.0
            && m_config.m_pacingBurstBytes > 0.0;
    SendResult sent;

    for (std::size_t index = 0; index < fragmentCount; ++index) {
        const std::size_t offset = index * fragmentSize;
        const std::size_t chunkBytes = std::min(fragmentSize, payloadBytes - offset);

        MessageHeader message{};
        fillMessageMagic(message.m_headId);
        message.m_messageType = static_cast<std::uint16_t>(MessageType::VideoStream);
        message.m_subType = static_cast<std::uint16_t>(frame.m_type);
        message.m_payloadLength = static_cast<std::uint32_t>(kUdpFrameFragmentHeaderWireSize + chunkBytes);

        UdpFrameFragmentHeader fragment{};
        fragment.m_frameSequence = frame.m_sequence;
        fragment.m_captureTimestampNs = frame.m_captureTimestampNs;
        fragment.m_encodeStartTimestampNs = frame.m_encodeStartTimestampNs;
        fragment.m_encodeEndTimestampNs = frame.m_encodeEndTimestampNs;
        fragment.m_transportSendTimestampNs = sendTimestampNs;
        fragment.m_framePayloadSize = static_cast<std::uint32_t>(payloadBytes);
        fragment.m_fragmentOffset = static_cast<std::uint32_t>(offset);
        fragment.m_fragmentIndex = static_cast<std::uint16_t>(index);
        fragment.m_fragmentCount = static_cast<std::uint16_t>(fragmentCount);
        fragment.m_fragmentRole = static_cast<std::uint16_t>(UdpFragmentRole::Data);
        // FEC 未启用时组号恒为 1 组，填 0；启用交织后由发送侧按
        // fragmentIndex % 组数 计算。
        fragment.m_parityGroupIndex = 0;
        fragment.m_originalPacketSequence = m_nextPacketSequence;
        fragment.m_packetSendTimestampNs = sendTimestampNs;
        fragment.m_transportFrameIndex = transportFrameIndex;
        fragment.m_frameFlags = frame.m_isKeyframe ? 1U : 0U;

        serializeMessageHeader(message, datagram.data());
        serializeUdpFrameFragmentHeader(fragment, datagram.data() + kMessageHeaderWireSize);
        std::memcpy(datagram.data() + kHeaderBytes, frame.m_payload.data() + offset, chunkBytes);

        if (pacingEnabled) {
            const std::uint64_t now = sendTimestampNs;
            std::uint64_t waitNs = 0;
            if (!m_bucket.tryAcquire(kHeaderBytes + chunkBytes, now, &waitNs)) {
                const std::uint64_t bounded =
                        std::min(waitNs, m_config.m_pacingMaxWaitNs);
                std::this_thread::sleep_for(std::chrono::nanoseconds(bounded));
                sent.m_paced = true;
                // 睡过之后仍取不到就直接发：整形是为了削峰，不是丢帧。
                m_bucket.tryAcquire(kHeaderBytes + chunkBytes,
                                    now + bounded, nullptr);
            }
        }

        if (!m_socket.sendTo(targetAddress, targetPort, datagram.data(),
                             kHeaderBytes + chunkBytes, errorMessage)) {
            // 发到一半失败时不再重试：UDP 没有部分成功的语义，
            // 接收端本来就靠缺失分片触发重传。
            return false;
        }
        ++sent.m_datagrams;
        sent.m_bytes += kHeaderBytes + chunkBytes;
        ++m_nextPacketSequence;
    }

    if (result != nullptr) {
        *result = sent;
    }
    return true;
}

int UdpSender::receiveFeedback(std::uint8_t *buffer, std::size_t capacity, int timeoutMs,
                               std::string *peerAddress, int *peerPort,
                               std::string *errorMessage) {
    return m_socket.recvFrom(buffer, capacity, timeoutMs, peerAddress, peerPort, errorMessage);
}


}  // namespace transport
}  // namespace sserver
