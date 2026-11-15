// ControlServer：TCP 控制面的端到端测试（假控制器 + 真实套接字）。
//
// 用假控制器把"协议 → 命令分发 → 响应格式化"整条路径跑通，不牵扯会话与
// 管线；真实会话链路由 server_app_test 覆盖。
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include "sserver/contracts/StreamingControl.h"
#include "sserver/control/ControlServer.h"
#include "support/Check.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

// ==================== 假控制器 ====================

class FakeController final : public sserver::contracts::StreamingController {
public:
    bool startStreaming(const sserver::contracts::StreamingRequest &request,
                        std::string *) override {
        m_lastStart = request;
        m_startCalls.fetch_add(1);
        return true;
    }

    bool stopStreaming(const std::string &sessionId, std::string *) override {
        m_lastStopped = sessionId;
        m_stopCalls.fetch_add(1);
        return true;
    }

    sserver::contracts::StreamingStatus getStreamingStatus(const std::string &sessionId) const override {
        sserver::contracts::StreamingStatus status;
        status.m_state = sessionId.empty() ? sserver::contracts::StreamingState::Idle
                                           : sserver::contracts::StreamingState::Running;
        status.m_sessionId = sessionId.empty() ? "none" : sessionId;
        status.m_transport = "udp";
        status.m_host = "127.0.0.1";
        status.m_port = 4321;
        status.m_profile = "balanced";
        status.m_mediaProfile = "default";
        status.m_codec = "h264";
        status.m_device = "auto";
        status.m_quality = "hd";
        return status;
    }

    void refreshStreamingLease(const std::string &sessionId, const std::string &peerIp,
                               int mediaPort) override {
        (void)sessionId;
        (void)peerIp;
        (void)mediaPort;
        m_leaseRefreshes.fetch_add(1);
    }

    bool requestKeyframe(const std::string &, std::string *, std::uint64_t *) override {
        m_keyframeCalls.fetch_add(1);
        return true;
    }

    std::atomic<int> m_startCalls{0};
    std::atomic<int> m_stopCalls{0};
    std::atomic<int> m_keyframeCalls{0};
    std::atomic<int> m_leaseRefreshes{0};
    sserver::contracts::StreamingRequest m_lastStart;
    std::string m_lastStopped;
};

// ==================== 极简 TCP 客户端 ====================

#if defined(_WIN32)
using ClientHandle = SOCKET;
using ClientLength = int;
constexpr ClientHandle kInvalidClient = INVALID_SOCKET;
#else
using ClientHandle = int;
using ClientLength = socklen_t;
constexpr ClientHandle kInvalidClient = -1;
#endif

void closeClient(ClientHandle handle) {
#if defined(_WIN32)
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

bool clientConnect(int port, ClientHandle *out) {
    const ClientHandle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kInvalidClient) {
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(handle, reinterpret_cast<const sockaddr *>(&address),
                  static_cast<ClientLength>(sizeof(address))) != 0) {
        closeClient(handle);
        return false;
    }
    *out = handle;
    return true;
}

// 发送一行并读回一行响应（到 '\n' 为止）。
bool requestLine(ClientHandle handle, const std::string &line, std::string *response) {
    const std::string payload = line + "\n";
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const int sent = static_cast<int>(::send(handle, payload.data() + offset,
                                                 static_cast<int>(payload.size() - offset), 0));
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    response->clear();
    for (int guard = 0; guard < 8192; ++guard) {
        char character = 0;
        const int received = static_cast<int>(::recv(handle, &character, 1, 0));
        if (received <= 0) {
            return false;
        }
        if (character == '\n') {
            return true;
        }
        response->push_back(character);
    }
    return false;
}

bool contains(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

// ==================== 用例 ====================

void testCommandsOverTcp() {
    FakeController controller;
    sserver::control::ControlServerConfig config;
    config.m_bindAddress = "127.0.0.1";
    config.m_port = 0;   // 内核分配，避免与同机其他服务冲突
    sserver::control::ControlServer server(controller, config);

    std::string error;
    CHECK(server.start(&error));
    CHECK(server.boundPort() > 0);

    ClientHandle client = kInvalidClient;
    CHECK(clientConnect(server.boundPort(), &client));

    std::string response;

    // STATUS：必须回 OK 且带全套状态字段。
    CHECK(requestLine(client, "STATUS session=s1", &response));
    CHECK(response.rfind("OK state=Running", 0) == 0);
    CHECK(contains(response, "port=4321"));
    CHECK(contains(response, "session=s1"));
    CHECK(controller.m_leaseRefreshes.load() == 1);

    // START：解析后的字段必须原样到达控制器。
    CHECK(requestLine(client,
                      "START transport=udp port=0 profile=auto session=s2 quality=hd enc=0",
                      &response));
    CHECK(response.rfind("OK", 0) == 0);
    CHECK(controller.m_startCalls.load() == 1);
    CHECK(controller.m_lastStart.m_sessionId == "s2");
    CHECK(controller.m_lastStart.m_profile == "auto");
    CHECK(controller.m_lastStart.m_port == 0);

    // KEYFRAME。
    CHECK(requestLine(client, "KEYFRAME session=s2", &response));
    CHECK(response.rfind("OK", 0) == 0);
    CHECK(controller.m_keyframeCalls.load() == 1);

    // CAPS：带档位列表。
    CHECK(requestLine(client, "CAPS", &response));
    CHECK(contains(response, "media_profiles="));

    // STOP。
    CHECK(requestLine(client, "STOP session=s2", &response));
    CHECK(response.rfind("OK", 0) == 0);
    CHECK(controller.m_stopCalls.load() == 1);
    CHECK(controller.m_lastStopped == "s2");

    // 畸形命令：错误码要具体，不能笼统报"失败"。
    CHECK(requestLine(client, "HELLO", &response));
    CHECK(response.rfind("ERR code=unknown_command", 0) == 0);

    CHECK(requestLine(client, "STATUS", &response));
    CHECK(response.rfind("ERR code=invalid_session", 0) == 0);

    CHECK(requestLine(client,
                      "START transport=udp port=0 profile=auto session=s3 quality=hd enc=0 extra=1",
                      &response));
    CHECK(response.rfind("ERR code=unknown_field", 0) == 0);

    closeClient(client);
    server.stop();
    CHECK(!server.running());
}

void testStopIsIdempotent() {
    FakeController controller;
    sserver::control::ControlServerConfig config;
    config.m_bindAddress = "127.0.0.1";
    config.m_port = 0;
    sserver::control::ControlServer server(controller, config);

    std::string error;
    CHECK(server.start(&error));
    server.stop();
    server.stop();
    CHECK(!server.running());
}

}  // namespace

int main() {
    testCommandsOverTcp();
    testStopIsIdempotent();
    return sserver::test::finish("control_server_test");
}
