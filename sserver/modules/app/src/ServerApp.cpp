// ============================================================
// 文件：ServerApp.cpp
// 模块：sserver::app
// 功能：会话表与每个会话的管线/发送器编排。START 建会话并绑定媒体端口，
//       客户端把接收报告发到该端口后服务端学会其地址，此后帧才真正发出；
//       STATUS 续租，超时未续租的会话由 reapExpiredSessions 回收。
// 知识点：共享指针保证回调期间对象存活、锁外停线程、地址注册、
//         发送结果的分类计数
// ============================================================

#include "sserver/app/ServerApp.h"

#include <utility>
#include <vector>

#include "sserver/base/MonotonicClock.h"
#include "sserver/pipeline/SyntheticCapture.h"
#include "sserver/transport/protocol/StreamProtocol.h"

namespace sserver {
namespace app {

struct ServerApp::Session {
    std::string m_sessionId;
    std::string m_device;
    std::string m_profile;
    std::string m_mediaProfile;
    std::string m_quality;
    bool m_encryption = false;

    contracts::StreamingState m_state = contracts::StreamingState::Idle;
    std::uint64_t m_lastLeaseNs = 0;
    int m_mediaPort = 0;

    std::unique_ptr<pipeline::StreamProducer> m_producer;
    std::unique_ptr<transport::UdpSender> m_sender;
    std::thread m_feedbackThread;
    std::atomic<bool> m_feedbackRunning{false};

    // 目标地址由反馈线程写、分发线程读。
    mutable std::mutex m_targetMutex;
    std::string m_targetAddress;
    int m_targetPort = 0;

    std::atomic<std::uint64_t> m_framesSent{0};
    std::atomic<std::uint64_t> m_framesSkipped{0};
    std::atomic<std::uint64_t> m_sendErrors{0};
};

ServerApp::ServerApp(appconfig::AppConfig config, ServerAppConfig appConfig)
        : m_config(std::move(config)), m_appConfig(std::move(appConfig)) {
}

ServerApp::~ServerApp() {
    shutdown();
}

// ==================== 会话生命周期 ====================

bool ServerApp::startStreaming(const contracts::StreamingRequest &request,
                               std::string *errorMessage) {
    const auto fail = [errorMessage](const std::string &message) {
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        return false;
    };

    if (request.m_transport != "udp") {
        return fail("unsupported transport");
    }
    if (!contracts::isValidSessionId(request.m_sessionId)) {
        return fail("invalid session id");
    }
    if (request.m_encryptionRequested) {
        // 加密要等 mediasec 落地；这里明确拒绝而不是静默降级成明文——
        // 让客户端以为链路加密了是最危险的一种"成功"。
        return fail("media encryption is not available yet");
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sessions.find(request.m_sessionId) != m_sessions.end()) {
            return fail("session already exists");
        }
    }

    auto session = std::make_shared<Session>();
    session->m_sessionId = request.m_sessionId;
    session->m_device = request.m_device.empty() ? m_config.m_capture.m_device : request.m_device;
    session->m_profile = request.m_profile;
    session->m_mediaProfile = request.m_mediaProfile;
    session->m_quality = request.m_quality;
    session->m_encryption = request.m_encryptionRequested;

    transport::UdpSenderConfig senderConfig;
    senderConfig.m_fragmentPayloadBytes = static_cast<std::size_t>(m_appConfig.m_fragmentPayloadBytes);
    session->m_sender = std::make_unique<transport::UdpSender>(senderConfig);
    if (!session->m_sender->open(m_appConfig.m_bindAddress, request.m_port, errorMessage)) {
        return false;
    }
    session->m_mediaPort = session->m_sender->boundPort();
    if (session->m_mediaPort <= 0) {
        return fail("failed to determine media port");
    }

    // 分发回调里要访问会话的发送器与目标地址。捕获 shared_ptr 而不是裸
    // this：会话被 stopStreaming 摘除时，正在执行的回调仍然安全。
    const std::shared_ptr<Session> sharedSession = session;
    const auto sink = [sharedSession](base::EncodedFramePtr frame) {
        std::string address;
        int port = 0;
        {
            std::lock_guard<std::mutex> lock(sharedSession->m_targetMutex);
            address = sharedSession->m_targetAddress;
            port = sharedSession->m_targetPort;
        }
        if (address.empty() || port <= 0) {
            // 客户端还没注册接收地址：丢掉这一帧比发给未知地址更安全。
            sharedSession->m_framesSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        transport::UdpSender::SendResult result;
        std::string error;
        if (sharedSession->m_sender->sendFrame(*frame, address, port, frame->m_sequence,
                                               base::monotonicNowNs(), &result, &error)) {
            sharedSession->m_framesSent.fetch_add(1, std::memory_order_relaxed);
        } else {
            sharedSession->m_sendErrors.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto source = std::make_unique<pipeline::SyntheticFrameSource>(
            m_appConfig.m_width, m_appConfig.m_height, m_appConfig.m_fps);
    auto encoder = std::make_unique<pipeline::SyntheticEncoder>();
    session->m_producer = std::make_unique<pipeline::StreamProducer>(
            std::move(source), std::move(encoder), m_config.m_codec);

    std::string startError;
    if (!session->m_producer->start(sink, &startError)) {
        return fail("failed to start pipeline: " + startError);
    }

    session->m_lastLeaseNs = base::monotonicNowNs();
    session->m_state = contracts::StreamingState::Running;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 竞态兜底：两次并发 START 同一会话时，后到的一方在插入前再确认一次。
        if (m_sessions.find(request.m_sessionId) != m_sessions.end()) {
            session->m_producer->stop();
            return fail("session already exists");
        }
        m_sessions.emplace(request.m_sessionId, session);
    }

    startFeedbackLoop(session);
    return true;
}

void ServerApp::startFeedbackLoop(const std::shared_ptr<Session> &session) {
    session->m_feedbackRunning.store(true);
    session->m_feedbackThread = std::thread([session]() {
        std::vector<std::uint8_t> buffer(2048, 0);
        while (session->m_feedbackRunning.load(std::memory_order_acquire)) {
            std::string peerAddress;
            int peerPort = 0;
            std::string error;
            const int received = session->m_sender->receiveFeedback(
                    buffer.data(), buffer.size(), 100, &peerAddress, &peerPort, &error);
            if (received <= 0) {
                continue;
            }
            transport::MessageHeader header{};
            if (!transport::parseMessageHeader(buffer.data(),
                                               static_cast<std::size_t>(received), &header)) {
                continue;
            }
            if (!transport::hasValidMessageMagic(header)) {
                continue;
            }
            if (header.m_messageType
                != static_cast<std::uint16_t>(transport::MessageType::KeepAlive)) {
                continue;
            }
            // 报告必须完整：半截报告里的地址不可信，宁可不注册。
            if (header.m_payloadLength < transport::kUdpReceiverReportWireSize) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(session->m_targetMutex);
                session->m_targetAddress = peerAddress;
                session->m_targetPort = peerPort;
            }
        }
    });
}

void ServerApp::stopSession(const std::shared_ptr<Session> &session) {
    if (session->m_producer != nullptr) {
        // 先停管线：它会 join 分发线程，保证不再有回调进入发送器。
        session->m_producer->stop();
    }
    session->m_feedbackRunning.store(false);
    if (session->m_feedbackThread.joinable()) {
        session->m_feedbackThread.join();
    }
    if (session->m_sender != nullptr) {
        session->m_sender->close();
    }
    session->m_state = contracts::StreamingState::Idle;
}

bool ServerApp::stopStreaming(const std::string &sessionId, std::string *errorMessage) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto iterator = m_sessions.find(sessionId);
        if (iterator == m_sessions.end()) {
            if (errorMessage != nullptr) {
                *errorMessage = "unknown session";
            }
            return false;
        }
        session = iterator->second;
        m_sessions.erase(iterator);
    }
    // 锁外停线程：停管线可能耗时以百毫秒计，持有会话表锁会挡住其他会话的
    // STATUS/START。
    stopSession(session);
    return true;
}

void ServerApp::shutdown() {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto &entry : m_sessions) {
            sessions.push_back(entry.second);
        }
        m_sessions.clear();
    }
    for (const auto &session : sessions) {
        stopSession(session);
    }
}

// ==================== 状态与租约 ====================

contracts::StreamingStatus ServerApp::buildStatus(const Session &session) const {
    contracts::StreamingStatus status;
    status.m_state = session.m_state;
    status.m_sessionId = session.m_sessionId;
    status.m_transport = "udp";
    status.m_host = m_appConfig.m_bindAddress;
    status.m_port = session.m_mediaPort;
    status.m_profile = session.m_profile;
    status.m_mediaProfile = session.m_mediaProfile.empty() ? "default" : session.m_mediaProfile;
    status.m_activeNetworkProfile = session.m_profile;
    status.m_codec = "h264";
    status.m_device = session.m_device;
    status.m_quality = session.m_quality;
    return status;
}

contracts::StreamingStatus ServerApp::getStreamingStatus(const std::string &sessionId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto iterator = m_sessions.find(sessionId);
    if (iterator == m_sessions.end()) {
        // 未命中返回 Idle 与默认字段：客户端据此判断"这个会话不存在"，
        // 而不是拿到一个看起来正常的状态。
        return contracts::StreamingStatus{};
    }
    return buildStatus(*iterator->second);
}

void ServerApp::refreshStreamingLease(const std::string &sessionId,
                                      const std::string &peerIp,
                                      int mediaPort) {
    (void)peerIp;
    (void)mediaPort;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto iterator = m_sessions.find(sessionId);
    if (iterator == m_sessions.end()) {
        return;
    }
    iterator->second->m_lastLeaseNs = base::monotonicNowNs();
}

void ServerApp::reapExpiredSessions() {
    const std::uint64_t now = base::monotonicNowNs();
    std::vector<std::shared_ptr<Session>> expired;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto iterator = m_sessions.begin(); iterator != m_sessions.end();) {
            const std::uint64_t lastLease = iterator->second->m_lastLeaseNs;
            if (now > lastLease && now - lastLease >= m_appConfig.m_sessionLeaseNs) {
                expired.push_back(iterator->second);
                iterator = m_sessions.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    // 客户端崩溃或断网时发不出 STOP，租约是唯一的兜底回收手段。
    for (const auto &session : expired) {
        stopSession(session);
    }
}

bool ServerApp::requestKeyframe(const std::string &sessionId,
                                std::string *errorMessage,
                                std::uint64_t *requestId) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto iterator = m_sessions.find(sessionId);
        if (iterator == m_sessions.end()) {
            if (errorMessage != nullptr) {
                *errorMessage = "unknown session";
            }
            return false;
        }
        session = iterator->second;
    }
    if (session->m_producer != nullptr) {
        session->m_producer->requestKeyframe();
    }
    if (requestId != nullptr) {
        *requestId = base::monotonicNowNs();
    }
    return true;
}

// ==================== 诊断 ====================

std::size_t ServerApp::sessionCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.size();
}

bool ServerApp::sessionSnapshot(const std::string &sessionId, SessionSnapshot *out) const {
    if (out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto iterator = m_sessions.find(sessionId);
    if (iterator == m_sessions.end()) {
        return false;
    }
    const Session &session = *iterator->second;
    out->m_sessionId = session.m_sessionId;
    out->m_state = session.m_state;
    out->m_mediaPort = session.m_mediaPort;
    {
        std::lock_guard<std::mutex> targetLock(session.m_targetMutex);
        out->m_targetRegistered = !session.m_targetAddress.empty() && session.m_targetPort > 0;
    }
    out->m_framesSent = session.m_framesSent.load(std::memory_order_relaxed);
    out->m_framesSkippedNoTarget = session.m_framesSkipped.load(std::memory_order_relaxed);
    out->m_sendErrors = session.m_sendErrors.load(std::memory_order_relaxed);
    return true;
}

}  // namespace app
}  // namespace sserver
