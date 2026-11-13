// ============================================================
// 文件：ServerApp.h
// 模块：sserver::app
// 功能：实现控制面的 StreamingController 接口：会话表、租约、每会话一条
//       管线与一个 UDP 发送器；从客户端报告数据报里学习接收地址。
// 知识点：依赖倒置（control 只认接口）、会话生命周期、地址注册、
//         跨线程共享会话状态
// ============================================================

#ifndef SSERVER_APP_SERVER_APP_H
#define SSERVER_APP_SERVER_APP_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "sserver/appconfig/AppConfig.h"
#include "sserver/contracts/StreamingControl.h"
#include "sserver/pipeline/StreamProducer.h"
#include "sserver/transport/udp/UdpSender.h"

namespace sserver {
namespace app {

struct ServerAppConfig {
    std::uint64_t m_sessionLeaseNs = 30ULL * 1000ULL * 1000ULL * 1000ULL;
    std::string m_bindAddress = "0.0.0.0";
    int m_fragmentPayloadBytes = 1200;
    // 现阶段只有合成源：接上 V4L2 后这两项改由采集设备协商结果决定。
    int m_width = 640;
    int m_height = 480;
    int m_fps = 25;
};

// 会话快照：给测试和诊断用，避免把内部结构暴露出去。
struct SessionSnapshot {
    std::string m_sessionId;
    contracts::StreamingState m_state = contracts::StreamingState::Idle;
    int m_mediaPort = 0;
    bool m_targetRegistered = false;
    std::uint64_t m_framesSent = 0;
    std::uint64_t m_framesSkippedNoTarget = 0;
    std::uint64_t m_sendErrors = 0;
};

class ServerApp final : public contracts::StreamingController {
public:
    explicit ServerApp(appconfig::AppConfig config,
                       ServerAppConfig appConfig = ServerAppConfig());
    ~ServerApp() override;

    ServerApp(const ServerApp &) = delete;
    ServerApp &operator=(const ServerApp &) = delete;

    // ==================== StreamingController ====================

    bool startStreaming(const contracts::StreamingRequest &request,
                        std::string *errorMessage = nullptr) override;

    bool stopStreaming(const std::string &sessionId,
                       std::string *errorMessage = nullptr) override;

    contracts::StreamingStatus getStreamingStatus(const std::string &sessionId) const override;

    void refreshStreamingLease(const std::string &sessionId,
                               const std::string &peerIp,
                               int mediaPort) override;

    void reapExpiredSessions() override;

    bool requestKeyframe(const std::string &sessionId,
                         std::string *errorMessage = nullptr,
                         std::uint64_t *requestId = nullptr) override;

    // ==================== 生命周期与诊断 ====================

    // 停止并回收全部会话。析构时自动调用。
    void shutdown();

    std::size_t sessionCount() const;

    // 未命中返回 false 且不改动 out。
    bool sessionSnapshot(const std::string &sessionId, SessionSnapshot *out) const;

private:
    struct Session;

    void startFeedbackLoop(const std::shared_ptr<Session> &session);
    void stopSession(const std::shared_ptr<Session> &session);
    contracts::StreamingStatus buildStatus(const Session &session) const;

    appconfig::AppConfig m_config;
    ServerAppConfig m_appConfig;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<Session>> m_sessions;
};

}  // namespace app
}  // namespace sserver

#endif  // SSERVER_APP_SERVER_APP_H
