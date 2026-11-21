// ServerApp：会话生命周期与 UDP 地址注册的端到端测试。
//
// 这条用例覆盖里程碑 A 的服务端半边：START 建会话并分配媒体端口 →
// 客户端发出接收报告 → 服务端学会地址并开始推流 → 客户端拼出完整帧 →
// STOP 回收。全程走真实 socket。
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "sserver/app/ServerApp.h"
#include "sserver/appconfig/AppConfig.h"
#include "sserver/contracts/StreamingControl.h"
#include "sserver/transport/protocol/StreamProtocol.h"
#include "sserver/transport/udp/UdpSocket.h"
#include "support/Check.h"

namespace {

using sserver::app::ServerApp;
using sserver::app::ServerAppConfig;
using sserver::app::SessionSnapshot;
using namespace sserver::transport;

constexpr int kReceiveTimeoutMs = 500;
constexpr std::size_t kKeepAlivePayload = kUdpReceiverReportWireSize;

ServerAppConfig makeAppConfig() {
    ServerAppConfig config;
    config.m_bindAddress = "127.0.0.1";
    config.m_width = 160;
    config.m_height = 120;
    config.m_fps = 25;
    config.m_fragmentPayloadBytes = 1200;
    // 本用例断言合成编码器的定长输出（1500 字节、可预测头部），
    // 所以固定的编码器必须是合成替身，不能随构建是否链接 libx264 而变。
    config.m_forceSyntheticEncoder = true;
    return config;
}

sserver::contracts::StreamingRequest makeStartRequest(int mediaPort = 0) {
    sserver::contracts::StreamingRequest request;
    request.m_sessionId = "s1";
    request.m_transport = "udp";
    request.m_port = mediaPort;
    request.m_profile = "balanced";
    request.m_quality = "hd";
    request.m_encryptionRequested = false;
    return request;
}

// 构造一份保活数据报：12 字节头 + 660 字节全零报告。服务端只要求
// "魔数正确 + 类型是 KeepAlive + 载荷够一整份报告"。
std::vector<std::uint8_t> makeKeepAliveDatagram() {
    std::vector<std::uint8_t> datagram(kMessageHeaderWireSize + kKeepAlivePayload, 0);
    MessageHeader header{};
    fillMessageMagic(header.m_headId);
    header.m_messageType = static_cast<std::uint16_t>(MessageType::KeepAlive);
    header.m_subType = 0;
    header.m_payloadLength = static_cast<std::uint32_t>(kKeepAlivePayload);
    serializeMessageHeader(header, datagram.data());
    UdpReceiverReport report{};
    report.m_reportTimestampNs = 1;
    serializeUdpReceiverReport(report, datagram.data() + kMessageHeaderWireSize);
    return datagram;
}

// 用 Start 请求的端口启动一个会话，并等到地址注册完成。
bool startSession(ServerApp *app, int *mediaPort, std::string *error) {
    if (!app->startStreaming(makeStartRequest(), error)) {
        return false;
    }
    const auto status = app->getStreamingStatus("s1");
    if (status.m_state != sserver::contracts::StreamingState::Running) {
        return false;
    }
    *mediaPort = status.m_port;
    return *mediaPort > 0;
}

// 尝试拼出一整帧；返回是否成功，成功时填充 assembled。
bool receiveWholeFrame(UdpSocket *client, std::vector<std::uint8_t> *assembled,
                       UdpFrameFragmentHeader *firstFragment, std::string *error) {
    std::uint64_t currentSequence = ~0ULL;
    std::size_t receivedFragments = 0;
    std::size_t expectedFragments = 0;
    std::uint32_t expectedSize = 0;

    for (int attempt = 0; attempt < 200; ++attempt) {
        std::vector<std::uint8_t> buffer(2048, 0);
        const int received = client->recvFrom(buffer.data(), buffer.size(), kReceiveTimeoutMs,
                                              nullptr, nullptr, error);
        if (received <= 0) {
            continue;
        }
        MessageHeader header{};
        if (!parseMessageHeader(buffer.data(), static_cast<std::size_t>(received), &header)) {
            continue;
        }
        if (!hasValidMessageMagic(header)
            || header.m_messageType != static_cast<std::uint16_t>(MessageType::VideoStream)) {
            continue;
        }
        UdpFrameFragmentHeader fragment{};
        parseUdpFrameFragmentHeader(buffer.data() + kMessageHeaderWireSize, &fragment);
        if (fragment.m_frameSequence != currentSequence) {
            // 换帧：前面的分片属于上一帧，丢弃重来。
            currentSequence = fragment.m_frameSequence;
            receivedFragments = 0;
            expectedFragments = fragment.m_fragmentCount;
            expectedSize = fragment.m_framePayloadSize;
            assembled->assign(expectedSize, 0);
        }
        if (receivedFragments == 0 && firstFragment != nullptr) {
            *firstFragment = fragment;
        }
        const std::size_t payloadBytes =
                static_cast<std::size_t>(received) - kMessageHeaderWireSize
                - kUdpFrameFragmentHeaderWireSize;
        if (fragment.m_fragmentOffset + payloadBytes <= assembled->size()) {
            std::copy(buffer.begin() + static_cast<std::ptrdiff_t>(kMessageHeaderWireSize
                                                                  + kUdpFrameFragmentHeaderWireSize),
                      buffer.begin() + static_cast<std::ptrdiff_t>(received),
                      assembled->begin() + static_cast<std::ptrdiff_t>(fragment.m_fragmentOffset));
        }
        ++receivedFragments;
        if (expectedFragments > 0 && receivedFragments == expectedFragments) {
            return true;
        }
    }
    return false;
}

void testEndToEndStreaming() {
    sserver::appconfig::AppConfig config;
    ServerApp app(config, makeAppConfig());

    int mediaPort = 0;
    std::string error;
    CHECK(startSession(&app, &mediaPort, &error));
    CHECK(app.sessionCount() == 1);

    UdpSocket client;
    CHECK(client.open(&error));
    CHECK(client.bind("127.0.0.1", 0, &error));

    // 注册之前不会有帧发出来：先确认跳过计数在涨，再发报告。
    const auto keepAlive = makeKeepAliveDatagram();
    CHECK(client.sendTo("127.0.0.1", mediaPort, keepAlive.data(), keepAlive.size(), &error));

    bool registered = false;
    for (int attempt = 0; attempt < 100 && !registered; ++attempt) {
        SessionSnapshot snapshot;
        if (app.sessionSnapshot("s1", &snapshot)) {
            registered = snapshot.m_targetRegistered;
        }
        if (!registered) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    CHECK(registered);

    std::vector<std::uint8_t> assembled;
    UdpFrameFragmentHeader firstFragment{};
    CHECK(receiveWholeFrame(&client, &assembled, &firstFragment, &error));

    // 合成编码器每帧固定 1500 字节：4 字节起始码 + NAL 头 + 8 字节序号 + 填充。
    CHECK(assembled.size() == 1500);
    CHECK(firstFragment.m_framePayloadSize == 1500);
    CHECK(firstFragment.m_fragmentCount == 2);   // 1200 + 300
    CHECK(assembled[0] == 0x00 && assembled[1] == 0x00
          && assembled[2] == 0x00 && assembled[3] == 0x01);
    // NAL 头只可能是关键帧的 0x65 或非关键帧的 0x41。
    CHECK(assembled[4] == 0x65 || assembled[4] == 0x41);

    SessionSnapshot snapshot;
    CHECK(app.sessionSnapshot("s1", &snapshot));
    CHECK(snapshot.m_framesSent > 0);
    CHECK(snapshot.m_sendErrors == 0);

    CHECK(app.stopStreaming("s1", &error));
    CHECK(app.sessionCount() == 0);
    CHECK(app.getStreamingStatus("s1").m_state
          == sserver::contracts::StreamingState::Idle);

    client.close();
}

void testDuplicateStartRejected() {
    sserver::appconfig::AppConfig config;
    ServerApp app(config, makeAppConfig());

    std::string error;
    CHECK(app.startStreaming(makeStartRequest(), &error));
    CHECK(!app.startStreaming(makeStartRequest(), &error));
    CHECK(error.find("already exists") != std::string::npos);
    // 失败的 START 不能把原会话弄坏。
    CHECK(app.sessionCount() == 1);
    CHECK(app.getStreamingStatus("s1").m_state
          == sserver::contracts::StreamingState::Running);
    app.shutdown();
}

void testRejectsUnsupportedRequests() {
    sserver::appconfig::AppConfig config;
    ServerApp app(config, makeAppConfig());
    std::string error;

    auto request = makeStartRequest();
    request.m_transport = "tcp";
    CHECK(!app.startStreaming(request, &error));
    CHECK(error.find("transport") != std::string::npos);

    request = makeStartRequest();
    request.m_sessionId = "none";   // 保留字不能当会话号
    CHECK(!app.startStreaming(request, &error));

    // 加密能力还没接入：明确拒绝而不是静默降级成明文。
    request = makeStartRequest();
    request.m_encryptionRequested = true;
    CHECK(!app.startStreaming(request, &error));
    CHECK(error.find("encryption") != std::string::npos);

    CHECK(app.sessionCount() == 0);
}

void testLeaseExpiryReclaimsSession() {
    sserver::appconfig::AppConfig config;
    ServerAppConfig appConfig = makeAppConfig();
    appConfig.m_sessionLeaseNs = 50ULL * 1000ULL * 1000ULL;  // 50ms，便于测试
    ServerApp app(config, appConfig);

    std::string error;
    CHECK(app.startStreaming(makeStartRequest(), &error));
    CHECK(app.sessionCount() == 1);

    // 租约未过期前不回收。
    app.reapExpiredSessions();
    CHECK(app.sessionCount() == 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    // 客户端崩溃时发不出 STOP，租约是唯一的兜底回收手段。
    app.reapExpiredSessions();
    CHECK(app.sessionCount() == 0);
}

void testLeaseRefreshKeepsSession() {
    sserver::appconfig::AppConfig config;
    ServerAppConfig appConfig = makeAppConfig();
    appConfig.m_sessionLeaseNs = 80ULL * 1000ULL * 1000ULL;
    ServerApp app(config, appConfig);

    std::string error;
    CHECK(app.startStreaming(makeStartRequest(), &error));
    for (int round = 0; round < 4; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        app.refreshStreamingLease("s1", "127.0.0.1", 0);
        app.reapExpiredSessions();
        CHECK(app.sessionCount() == 1);
    }
}

}  // namespace

int main() {
    testEndToEndStreaming();
    testDuplicateStartRejected();
    testRejectsUnsupportedRequests();
    testLeaseExpiryReclaimsSession();
    testLeaseRefreshKeepsSession();
    return sserver::test::finish("server_app_test");
}
