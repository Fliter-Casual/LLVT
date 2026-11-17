// ============================================================
// 文件：main.cpp
// 模块：sclient::app
// 功能：命令行客户端：连接服务端 → START → 注册媒体地址 → 持续收帧并
//       每 2 秒续租 → 退出时 STOP。
// 知识点：命令行参数解析、连接/停止的顺序、保活与统计节流
// ============================================================

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "sclient/base/MonotonicClock.h"
#include "sclient/control/ControlClient.h"
#include "sclient/transport/protocol/StreamProtocol.h"
#include "sclient/transport/udp/UdpReceiver.h"

namespace {

// 退出标志：收到 SIGINT 或 SIGTERM 时设置为 1。
volatile std::sig_atomic_t g_stopRequested = 0;

// 信号处理函数：收到 SIGINT 或 SIGTERM 时设置退出标志。
void handleSignal(int) {
    g_stopRequested = 1;
}

// 打印帮助信息。
void printUsage(const char *program) {
    std::fprintf(stderr,
                 "usage: %s [--host <ip>] [--control-port <port>] [--session <id>]"
                 " [--frames <n>]\n"
                 "  --host          服务端地址，默认 127.0.0.1\n"
                 "  --control-port  控制端口，默认 9000\n"
                 "  --session       会话号，默认 cli-1\n"
                 "  --frames        收到这么多帧后退出，0 表示一直收（默认 0）\n",
                 program);
}

// 注册数据报：12 字节消息头 + 660 字节接收报告。
// 服务端从这条数据报的源地址学习接收端，所以必须从收流的套接字发出。
std::vector<std::uint8_t> makeRegistrationReport() {
    std::vector<std::uint8_t> datagram(
            sclient::transport::kMessageHeaderWireSize
                    + sclient::transport::kUdpReceiverReportWireSize,
            0);
    sclient::transport::MessageHeader header{};
    sclient::transport::fillMessageMagic(header.m_headId);
    header.m_messageType =
            static_cast<std::uint16_t>(sclient::transport::MessageType::KeepAlive);
    header.m_subType = 0;
    header.m_payloadLength = sclient::transport::kUdpReceiverReportWireSize;
    sclient::transport::serializeMessageHeader(header, datagram.data());

    sclient::transport::UdpReceiverReport report{};
    report.m_reportTimestampNs = sclient::base::monotonicNowNs();
    sclient::transport::serializeUdpReceiverReport(
            report, datagram.data() + sclient::transport::kMessageHeaderWireSize);
    return datagram;
}

}  // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::string host = "127.0.0.1";
    int controlPort = 9000;
    std::string sessionId = "cli-1";
    int maxFrames = 0;
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (std::strcmp(argument, "--host") == 0 && index + 1 < argc) {
            host = argv[++index];
        } else if (std::strcmp(argument, "--control-port") == 0 && index + 1 < argc) {
            controlPort = std::atoi(argv[++index]);
        } else if (std::strcmp(argument, "--session") == 0 && index + 1 < argc) {
            sessionId = argv[++index];
        } else if (std::strcmp(argument, "--frames") == 0 && index + 1 < argc) {
            maxFrames = std::atoi(argv[++index]);
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }

    // 连接服务端。
    sclient::control::ControlClient control;
    std::string error;
    if (!control.connect(host, controlPort, 3000, &error)) {
        std::fprintf(stderr, "connect failed: %s\n", error.c_str());
        return 1;
    }

    // 发送 START 命令。
    sclient::control::ControlResponse response;
    const std::string startCommand = "START transport=udp port=0 profile=balanced session="
            + sessionId + " quality=hd enc=0";
    if (!control.execute(startCommand, &response, 15000, &error)) {
        std::fprintf(stderr, "START failed: %s\n", error.c_str());
        return 1;
    }
    if (!response.m_ok) {
        // 服务端拒绝时错误码是具体原因（invalid_session / start_failed ...），
        // 直接打印，不做二次猜测。
        std::fprintf(stderr, "START rejected: code=%s message=%s\n",
                     response.m_errorCode.c_str(), response.m_errorMessage.c_str());
        return 1;
    }

    // 媒体端口以服务端回传为准：它是服务端动态分配的，客户端不能自己想当然。
    const int mediaPort = response.intField("port", 0);
    if (mediaPort <= 0) {
        std::fprintf(stderr, "START response carried no usable media port\n");
        control.execute("STOP session=" + sessionId, &response, 2000, nullptr);
        return 1;
    }
    std::printf("session=%s media_port=%d codec=%s\n",
                sessionId.c_str(), mediaPort, response.field("codec").c_str());

    // 注册数据报。
    sclient::transport::UdpReceiver receiver;
    if (!receiver.open("0.0.0.0", 0, &error)) {
        std::fprintf(stderr, "media socket failed: %s\n", error.c_str());
        control.execute("STOP session=" + sessionId, &response, 2000, nullptr);
        return 1;
    }
    // 发送注册数据报。
    const auto registration = makeRegistrationReport();
    if (!receiver.sendDatagram(host, mediaPort, registration.data(), registration.size(),
                               &error)) {
        std::fprintf(stderr, "registration failed: %s\n", error.c_str());
        control.execute("STOP session=" + sessionId, &response, 2000, nullptr);
        return 1;
    }

    // 接收数据。
    // 从服务端接收数据，统计帧数、关键帧数、字节数、重复帧数、无效帧数。
    std::uint64_t frames = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t bytes = 0;
    std::uint64_t lastLeaseNs = sclient::base::monotonicNowNs();
    std::uint64_t lastStatsNs = lastLeaseNs;
    std::uint64_t framesAtLastStats = 0;

    constexpr std::uint64_t kLeaseIntervalNs = 2ULL * 1000ULL * 1000ULL * 1000ULL;
    constexpr std::uint64_t kStatsIntervalNs = 1000ULL * 1000ULL * 1000ULL;

    while (g_stopRequested == 0) {
        sclient::transport::ReceivedFrame frame;
        if (receiver.receiveFrame(&frame, 100, &error)) {
            ++frames;
            bytes += frame.m_payload.size();
            if ((frame.m_metadata.m_frameFlags & 1U) != 0) {
                ++keyframes;
            }
        }

        const std::uint64_t now = sclient::base::monotonicNowNs();
        if (now - lastLeaseNs >= kLeaseIntervalNs) {
            lastLeaseNs = now;
            // STATUS 兼续租：服务端 30 秒租约，2 秒一次即使丢一两条也够。
            const std::string statusCommand = "STATUS session=" + sessionId
                    + " media_port=" + std::to_string(receiver.boundPort());
            sclient::control::ControlResponse leaseResponse;
            std::string leaseError;
            if (!control.execute(statusCommand, &leaseResponse, 3000, &leaseError)) {
                std::fprintf(stderr, "keepalive failed: %s\n", leaseError.c_str());
                break;
            }
        }
        if (now - lastStatsNs >= kStatsIntervalNs) {
            const auto assemblerStats = receiver.stats();
            std::printf("frames=%llu (+%llu/s) keyframes=%llu bytes=%llu dup=%llu invalid=%llu\n",
                        static_cast<unsigned long long>(frames),
                        static_cast<unsigned long long>(frames - framesAtLastStats),
                        static_cast<unsigned long long>(keyframes),
                        static_cast<unsigned long long>(bytes),
                        static_cast<unsigned long long>(assemblerStats.m_duplicateFragments),
                        static_cast<unsigned long long>(assemblerStats.m_invalidFragments));
            std::fflush(stdout);
            framesAtLastStats = frames;
            lastStatsNs = now;
        }

        if (maxFrames > 0 && frames >= static_cast<std::uint64_t>(maxFrames)) {
            break;
        }
    }

    // 尽力释放本会话：失败只影响服务端回收时间（租约兜底），不阻塞退出。
    sclient::control::ControlResponse stopResponse;
    if (!control.execute("STOP session=" + sessionId, &stopResponse, 2000, nullptr)) {
        std::fprintf(stderr, "warning: STOP was not acknowledged\n");
    }
    control.close();

    std::printf("done: frames=%llu keyframes=%llu bytes=%llu\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(keyframes),
                static_cast<unsigned long long>(bytes));
    return frames > 0 ? 0 : 1;
}