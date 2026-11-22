// ============================================================
// 文件：GuiRunnerQt.cpp
// 模块：sclient::app
// 功能：窗口版客户端：Qt 事件循环里用定时器轮询收流、解码并提交渲染，
//       另有一个定时器每 2 秒续租；关窗时释放会话。
// 知识点：QSurfaceFormat 默认格式必须先于 QApplication 设置、定时器驱动的
//         收流循环、GUI 线程单线程模型（无跨线程图形访问）
// ============================================================

#include "sclient/app/GuiRunner.h"

#include <QApplication>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <vector>

#include "sclient/base/MonotonicClock.h"
#include "sclient/control/ControlClient.h"
#include "sclient/decoding/VideoDecoder.h"
#include "sclient/rendering/Renderer.h"
#include "sclient/transport/protocol/StreamProtocol.h"
#include "sclient/transport/udp/UdpReceiver.h"

namespace sclient {
namespace app {

namespace {

// 注册数据报：12 字节头 + 660 字节报告。必须从收流套接字发出——
// 服务端按数据报源地址学习接收端。
std::vector<std::uint8_t> makeRegistrationReport() {
    std::vector<std::uint8_t> datagram(
            transport::kMessageHeaderWireSize + transport::kUdpReceiverReportWireSize, 0);
    transport::MessageHeader header{};
    transport::fillMessageMagic(header.m_headId);
    header.m_messageType =
            static_cast<std::uint16_t>(transport::MessageType::KeepAlive);
    header.m_payloadLength = transport::kUdpReceiverReportWireSize;
    transport::serializeMessageHeader(header, datagram.data());

    transport::UdpReceiverReport report{};
    report.m_reportTimestampNs = base::monotonicNowNs();
    transport::serializeUdpReceiverReport(
            report, datagram.data() + transport::kMessageHeaderWireSize);
    return datagram;
}

}  // namespace

bool guiAvailable() {
    return true;
}

int runGuiClient(const GuiOptions &options) {
    // 核心 profile 必须在 QApplication 构造之前设为默认，否则控件创建时
    // 已经拿到兼容 profile 的上下文了。
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(0);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);

    int argc = 1;
    char programName[] = "sclient";
    char *argv[] = {programName, nullptr};
    QApplication application(argc, argv);

    control::ControlClient control;
    std::string error;
    if (!control.connect(options.m_host, options.m_controlPort, 3000, &error)) {
        std::fprintf(stderr, "connect failed: %s\n", error.c_str());
        return 1;
    }

    control::ControlResponse response;
    const std::string startCommand = "START transport=udp port=0 profile=balanced session="
            + options.m_sessionId + " quality=hd enc=0";
    if (!control.execute(startCommand, &response, 15000, &error)) {
        std::fprintf(stderr, "START failed: %s\n", error.c_str());
        return 1;
    }
    if (!response.m_ok) {
        std::fprintf(stderr, "START rejected: code=%s message=%s\n",
                     response.m_errorCode.c_str(), response.m_errorMessage.c_str());
        return 1;
    }
    const int mediaPort = response.intField("port", 0);
    if (mediaPort <= 0) {
        std::fprintf(stderr, "START response carried no usable media port\n");
        control.execute("STOP session=" + options.m_sessionId, &response, 2000, nullptr);
        return 1;
    }

    transport::UdpReceiver receiver;
    if (!receiver.open("0.0.0.0", 0, &error)) {
        std::fprintf(stderr, "media socket failed: %s\n", error.c_str());
        control.execute("STOP session=" + options.m_sessionId, &response, 2000, nullptr);
        return 1;
    }
    const auto registration = makeRegistrationReport();
    if (!receiver.sendDatagram(options.m_host, mediaPort, registration.data(),
                               registration.size(), &error)) {
        std::fprintf(stderr, "registration failed: %s\n", error.c_str());
        control.execute("STOP session=" + options.m_sessionId, &response, 2000, nullptr);
        return 1;
    }

    decoding::VideoDecoder decoder;
    const std::string codecName =
            response.field("codec").empty() ? std::string("h264") : response.field("codec");
    if (!decoder.initialize(codecName, &error)) {
        std::fprintf(stderr, "decoder init failed: %s\n", error.c_str());
        control.execute("STOP session=" + options.m_sessionId, &response, 2000, nullptr);
        return 1;
    }

    rendering::Renderer renderer;
    if (!renderer.initialize(&error) || renderer.widget() == nullptr) {
        std::fprintf(stderr, "renderer init failed: %s\n", error.c_str());
        control.execute("STOP session=" + options.m_sessionId, &response, 2000, nullptr);
        return 1;
    }
    // 色彩矩阵按服务端回显的元数据选：用错会让肤色发绿或发紫。
    renderer.setColorimetry(response.field("colorimetry"));

    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    // 边距清零：让视频铺满窗口，多余部分是黑边而不是白边。
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(renderer.widget());
    window.setWindowTitle(QString("sclient - %1:%2 session=%3")
                                  .arg(QString::fromStdString(options.m_host))
                                  .arg(mediaPort)
                                  .arg(QString::fromStdString(options.m_sessionId)));
    window.resize(960, 540);
    window.show();

    std::uint64_t receivedFrames = 0;
    std::uint64_t decodedFrames = 0;
    std::vector<decoding::DecodedFrame> decodedOutput;

    QTimer frameTimer;
    QObject::connect(&frameTimer, &QTimer::timeout, [&]() {
        transport::ReceivedFrame frame;
        // 1ms 超时：既不在 GUI 线程上长时间阻塞，也保证每次事件循环都能
        // 处理一批到达的数据报。
        if (!receiver.receiveFrame(&frame, 1, nullptr)) {
            return;
        }
        ++receivedFrames;
        decodedOutput.clear();
        if (!decoder.decodePacket(frame.m_payload.data(), frame.m_payload.size(),
                                  &decodedOutput, nullptr)) {
            return;
        }
        if (!decodedOutput.empty()) {
            decodedFrames += decodedOutput.size();
            // 解码器可能一次产出多帧；只显示最新的一帧，旧帧没有价值。
            renderer.submitFrame(decodedOutput.back());
        }
    });
    frameTimer.start(5);

    QTimer leaseTimer;
    QObject::connect(&leaseTimer, &QTimer::timeout, [&]() {
        const std::string statusCommand = "STATUS session=" + options.m_sessionId
                + " media_port=" + std::to_string(receiver.boundPort());
        control::ControlResponse leaseResponse;
        control.execute(statusCommand, &leaseResponse, 3000, nullptr);
    });
    leaseTimer.start(2000);

    const int exitCode = application.exec();

    frameTimer.stop();
    leaseTimer.stop();
    control.execute("STOP session=" + options.m_sessionId, &response, 2000, nullptr);
    control.close();
    std::printf("gui exit: received=%llu decoded=%llu\n",
                static_cast<unsigned long long>(receivedFrames),
                static_cast<unsigned long long>(decodedFrames));
    return exitCode;
}

}  // namespace app
}  // namespace sclient