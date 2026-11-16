// ============================================================
// 文件：UdpReceiver.cpp
// 模块：sclient::transport
// 功能：收包循环的实现：逐包校验、解析头部、喂给组装器；剩余超时预算
//       在多个数据报之间分摊，不会因为一直在收无效包而不返回。
// 知识点：单调时钟计时、校验顺序、无效包计数与丢弃
// ============================================================

#include "sclient/transport/udp/UdpReceiver.h"

#include <chrono>

#include "sclient/base/MonotonicClock.h"

namespace sclient {
namespace transport {

namespace {

// 缓冲区要装得下最大数据报（12 + 84 + 分片载荷）。64 KiB 是 UDP 的
// 理论上限，留足余量后一次读全，不会截断。
constexpr std::size_t kReceiveBufferBytes = 65536;

}  // namespace

UdpReceiver::UdpReceiver(const UdpFrameAssembler::Options &options)
        : m_assembler(options), m_buffer(kReceiveBufferBytes, 0) {
}

bool UdpReceiver::open(const std::string &bindAddress, int port, std::string *errorMessage) {
    return m_socket.open(errorMessage) && m_socket.bind(bindAddress, port, errorMessage);
}

bool UdpReceiver::receiveFrame(ReceivedFrame *frame, int timeoutMs, std::string *errorMessage) {
    if (frame == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "receiveFrame requires an output frame";
        }
        return false;
    }
    if (!m_socket.isOpen()) {
        if (errorMessage != nullptr) {
            *errorMessage = "receiver socket is not open";
        }
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::uint64_t lastPruneNs = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
        const int waitMs = static_cast<int>(remaining > 0 ? remaining : 1);

        std::string error;
        const int received = m_socket.recvFrom(m_buffer.data(), m_buffer.size(), waitMs,
                                               nullptr, nullptr, &error);
        const std::uint64_t nowNs = base::monotonicNowNs();

        // 每 10ms 剪一次半截帧：收包频率高时不必每包都遍历记录表。
        if (lastPruneNs == 0 || nowNs - lastPruneNs >= 10000000ULL) {
            m_assembler.prune(nowNs);
            lastPruneNs = nowNs;
        }

        if (received < 0) {
            if (errorMessage != nullptr) {
                *errorMessage = error;
            }
            return false;
        }
        if (received == 0) {
            continue;  // 本次等待超时，回到循环头部检查总预算
        }

        MessageHeader header{};
        if (!parseMessageHeader(m_buffer.data(), static_cast<std::size_t>(received), &header)) {
            continue;
        }
        if (!hasValidMessageMagic(header)) {
            continue;
        }
        if (header.m_messageType != static_cast<std::uint16_t>(MessageType::VideoStream)) {
            // 保活确认等非视频消息由反馈路径处理，这里直接跳过。
            continue;
        }
        if (header.m_payloadLength < kUdpFrameFragmentHeaderWireSize
            || header.m_payloadLength
                       > static_cast<std::uint32_t>(received - kMessageHeaderWireSize)) {
            continue;
        }

        const std::uint8_t *fragmentStart = m_buffer.data() + kMessageHeaderWireSize;
        UdpFrameFragmentHeader fragment{};
        parseUdpFrameFragmentHeader(fragmentStart, &fragment);
        const std::uint8_t *payloadStart = fragmentStart + kUdpFrameFragmentHeaderWireSize;
        const std::size_t payloadBytes =
                static_cast<std::size_t>(header.m_payloadLength) - kUdpFrameFragmentHeaderWireSize;

        if (m_assembler.addFragment(fragment, payloadStart, payloadBytes, nowNs, frame)) {
            return true;
        }
    }
    return false;
}

}  // namespace transport
}  // namespace sclient
