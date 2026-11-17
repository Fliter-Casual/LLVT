// ControlClient：连接、命令往返与超时的端到端测试。
//
// 测试里起一个极简的假服务端（监听 → 接受 → 读一行 → 回一行），
// 验证的是客户端这一侧：连接超时、读取超时、响应解析与错误分类。
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "sclient/control/ControlClient.h"
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

#if defined(_WIN32)
using NativeHandle = SOCKET;
using LengthType = int;
constexpr NativeHandle kInvalidHandle = INVALID_SOCKET;
#else
using NativeHandle = int;
using LengthType = socklen_t;
constexpr NativeHandle kInvalidHandle = -1;
#endif

void closeHandle(NativeHandle handle) {
#if defined(_WIN32)
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

// 假服务端直接调 ::socket，所以本进程必须先完成 winsock 初始化；
// 客户端内部会自己初始化，但那发生在 connect 时，已经晚了。
bool ensurePlatformInitialized() {
#if defined(_WIN32)
    static const bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
#else
    return true;
#endif
}

// 极简假服务端：每接受一个连接，按预设脚本逐行回复，然后关闭。
class FakeServer {
public:
    // replies 的第 i 条对应客户端第 i 条命令；空串表示不回复（触发读取超时）。
    explicit FakeServer(std::vector<std::string> replies) : m_replies(std::move(replies)) {}

    bool start() {
        m_listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listener == kInvalidHandle) {
            return false;
        }
        int reuse = 1;
        setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);
        if (::bind(m_listener, reinterpret_cast<const sockaddr *>(&local),
                   static_cast<LengthType>(sizeof(local))) != 0) {
            return false;
        }
        if (::listen(m_listener, 1) != 0) {
            return false;
        }
        sockaddr_in actual{};
        LengthType length = static_cast<LengthType>(sizeof(actual));
        getsockname(m_listener, reinterpret_cast<sockaddr *>(&actual), &length);
        m_port = static_cast<int>(ntohs(actual.sin_port));
        m_thread = std::thread([this]() { serve(); });
        return true;
    }

    int port() const { return m_port; }

    std::vector<std::string> receivedCommands() const { return m_received; }

    void join() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    ~FakeServer() {
        join();
        if (m_listener != kInvalidHandle) {
            closeHandle(m_listener);
        }
    }

private:
    void serve() {
        const NativeHandle connection = ::accept(m_listener, nullptr, nullptr);
        if (connection == kInvalidHandle) {
            return;
        }
        for (const std::string &reply : m_replies) {
            std::string line;
            while (true) {
                char character = 0;
                const int received = static_cast<int>(::recv(connection, &character, 1, 0));
                if (received <= 0) {
                    closeHandle(connection);
                    return;
                }
                if (character == '\n') {
                    break;
                }
                line.push_back(character);
            }
            m_received.push_back(line);
            if (reply.empty()) {
                // 不回复：让客户端走到读取超时分支。
                continue;
            }
            const std::string payload = reply + "\n";
            ::send(connection, payload.data(), static_cast<int>(payload.size()), 0);
        }
        closeHandle(connection);
    }

    std::vector<std::string> m_replies;
    std::vector<std::string> m_received;
    NativeHandle m_listener = kInvalidHandle;
    int m_port = 0;
    std::thread m_thread;
};

void testCommandRoundTrip() {
    FakeServer server({"OK state=Running port=7777 session=s1",
                       "ERR code=stop_failed message=unknown_session"});
    CHECK(server.start());

    sclient::control::ControlClient client;
    std::string error;
    CHECK(client.connect("127.0.0.1", server.port(), 1000, &error));

    sclient::control::ControlResponse response;
    CHECK(client.execute("STATUS session=s1", &response, 1000, &error));
    CHECK(response.m_ok);
    CHECK(response.intField("port") == 7777);

    CHECK(client.execute("STOP session=s1", &response, 1000, &error));
    // 传输成功但业务失败：execute 返回 true，成败看 m_ok。
    CHECK(!response.m_ok);
    CHECK(response.m_errorCode == "stop_failed");

    client.close();
    server.join();

    const auto commands = server.receivedCommands();
    CHECK(commands.size() == 2);
    CHECK(commands[0] == "STATUS session=s1");
    CHECK(commands[1] == "STOP session=s1");
}

void testReadTimeout() {
    // 服务端接受连接但一直不回复：客户端必须按超时返回，而不是永远挂着。
    FakeServer server({""});
    CHECK(server.start());

    sclient::control::ControlClient client;
    std::string error;
    CHECK(client.connect("127.0.0.1", server.port(), 1000, &error));

    sclient::control::ControlResponse response;
    const bool ok = client.execute("STATUS session=s1", &response, 100, &error);
    CHECK(!ok);
    CHECK(!error.empty());

    client.close();
}

void testConnectToClosedPort() {
    // 先占一个端口再释放，得到"几乎肯定没人监听"的端口号。
    const NativeHandle probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);
    ::bind(probe, reinterpret_cast<const sockaddr *>(&local),
           static_cast<LengthType>(sizeof(local)));
    sockaddr_in actual{};
    LengthType length = static_cast<LengthType>(sizeof(actual));
    getsockname(probe, reinterpret_cast<sockaddr *>(&actual), &length);
    const int closedPort = static_cast<int>(ntohs(actual.sin_port));
    closeHandle(probe);

    sclient::control::ControlClient client;
    std::string error;
    CHECK(!client.connect("127.0.0.1", closedPort, 300, &error));
    CHECK(!error.empty());
    CHECK(!client.isConnected());
}

void testExecuteWithoutConnection() {
    sclient::control::ControlClient client;
    sclient::control::ControlResponse response;
    std::string error;
    CHECK(!client.execute("CAPS", &response, 100, &error));
    CHECK(error.find("not open") != std::string::npos);
}

}  // namespace

int main() {
    CHECK(ensurePlatformInitialized());
    testCommandRoundTrip();
    testReadTimeout();
    testConnectToClosedPort();
    testExecuteWithoutConnection();
    return sclient::test::finish("control_client_test");
}
