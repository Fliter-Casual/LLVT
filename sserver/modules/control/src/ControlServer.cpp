// ============================================================
// 文件：ControlServer.cpp
// 模块：sserver::control
// 功能：TCP 监听 + 逐行处理。系统调用差异（POSIX / winsock）集中在本文件；
//       协议解析与响应格式化复用 ControlProtocol 的纯函数。
// 知识点：select 限时等待、监听套接字关闭即停止、按行读取与长度上限、
//         单行响应拼装
// ============================================================

#include "sserver/control/ControlServer.h"

#include <cstring>
#include <mutex>
#include <utility>

#include "sserver/control/ControlProtocol.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sserver {
namespace control {

namespace {

#if defined(_WIN32)
using NativeHandle = SOCKET;
constexpr NativeHandle kInvalidHandle = INVALID_SOCKET;
using LengthType = int;
#else
using NativeHandle = int;
constexpr NativeHandle kInvalidHandle = -1;
using LengthType = socklen_t;
#endif

NativeHandle nativeOf(std::intptr_t handle) {
    return static_cast<NativeHandle>(handle);
}

std::intptr_t wrap(NativeHandle handle) {
    return static_cast<std::intptr_t>(handle);
}

int lastSocketError() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

// 与 transport 模块里的那份是同一套逻辑：control 不该为了一个 WSAStartup
// 去链接媒体传输模块，所以这里单独做一次，代价是十来行重复。
bool ensurePlatformInitialized(std::string *errorMessage) {
#if defined(_WIN32)
    static std::once_flag once;
    static bool initialized = false;
    static std::string failure;
    std::call_once(once, [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            failure = "WSAStartup failed, error " + std::to_string(WSAGetLastError());
            return;
        }
        initialized = true;
    });
    if (!initialized) {
        if (errorMessage != nullptr) {
            *errorMessage = failure;
        }
        return false;
    }
#else
    (void)errorMessage;
#endif
    return true;
}

void closeHandle(NativeHandle handle) {
#if defined(_WIN32)
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

// 发一行响应。控制协议一请求一响应，写完不需要 flush 语义：TCP 自己保证。
bool writeAll(NativeHandle handle, const std::string &text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const int sent = static_cast<int>(::send(
                handle, text.data() + offset, static_cast<int>(text.size() - offset), 0));
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

}  // namespace

ControlServer::ControlServer(contracts::StreamingController &controller,
                             ControlServerConfig config)
        : m_controller(controller), m_config(std::move(config)) {
}

ControlServer::~ControlServer() {
    stop();
}

bool ControlServer::start(std::string *errorMessage) {
    if (m_running.load()) {
        if (errorMessage != nullptr) {
            *errorMessage = "control server already running";
        }
        return false;
    }
    if (!ensurePlatformInitialized(errorMessage)) {
        return false;
    }

    const NativeHandle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidHandle) {
        if (errorMessage != nullptr) {
            *errorMessage = "failed to create control socket, error "
                    + std::to_string(lastSocketError());
        }
        return false;
    }

    // SO_REUSEADDR：重启服务端时不必等 TIME_WAIT 结束。
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&reuse), sizeof(reuse));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<std::uint16_t>(m_config.m_port));
    const std::string bindAddress =
            m_config.m_bindAddress.empty() ? std::string("0.0.0.0") : m_config.m_bindAddress;
    if (inet_pton(AF_INET, bindAddress.c_str(), &local.sin_addr) != 1) {
        closeHandle(listener);
        if (errorMessage != nullptr) {
            *errorMessage = "invalid bind address '" + bindAddress + "'";
        }
        return false;
    }
    if (::bind(listener, reinterpret_cast<const sockaddr *>(&local),
               static_cast<LengthType>(sizeof(local))) != 0) {
        closeHandle(listener);
        if (errorMessage != nullptr) {
            *errorMessage = "failed to bind control port " + std::to_string(m_config.m_port)
                    + ", error " + std::to_string(lastSocketError());
        }
        return false;
    }
    if (::listen(listener, 8) != 0) {
        closeHandle(listener);
        if (errorMessage != nullptr) {
            *errorMessage = "failed to listen, error " + std::to_string(lastSocketError());
        }
        return false;
    }

    sockaddr_in actual{};
    LengthType length = static_cast<LengthType>(sizeof(actual));
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&actual), &length) == 0) {
        m_boundPort = static_cast<int>(ntohs(actual.sin_port));
    }

    m_listenHandle = wrap(listener);
    m_running.store(true);
    m_thread = std::thread(&ControlServer::acceptLoop, this);
    return true;
}

void ControlServer::stop() {
    if (m_running.exchange(false)) {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    } else if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_listenHandle >= 0) {
        closeHandle(nativeOf(m_listenHandle));
        m_listenHandle = -1;
    }
}

void ControlServer::acceptLoop() {
    const NativeHandle listener = nativeOf(m_listenHandle);
    while (m_running.load(std::memory_order_acquire)) {
        // 用 select 限时等待而不是阻塞在 accept 上：stop() 只需要置标志，
        // 循环最多再睡一个超时周期就退出，不必用自唤醒管道。
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listener, &readSet);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200 * 1000;
        const int ready = ::select(static_cast<int>(listener + 1), &readSet, nullptr, nullptr,
                                   &timeout);
        if (ready <= 0) {
            continue;
        }
        sockaddr_in peer{};
        LengthType peerLength = static_cast<LengthType>(sizeof(peer));
        const NativeHandle connection = ::accept(
                listener, reinterpret_cast<sockaddr *>(&peer), &peerLength);
        if (connection == kInvalidHandle) {
            continue;
        }
        char peerText[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, peerText, sizeof(peerText));
        handleConnection(wrap(connection), peerText);
    }
}

void ControlServer::handleConnection(std::intptr_t handle, const std::string &peerAddress) {
    const NativeHandle connection = nativeOf(handle);
    std::string line;
    bool overflow = false;
    while (m_running.load(std::memory_order_acquire)) {
        char character = 0;
        const int received = static_cast<int>(::recv(connection, &character, 1, 0));
        if (received <= 0) {
            break;  // 对端关闭或出错
        }
        if (character == '\n') {
            // 容忍 CRLF 客户端：echo 在 Windows 上会带 \r。
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::string response = handleLine(line, peerAddress);
            if (!writeAll(connection, response + "\n")) {
                break;
            }
            line.clear();
            overflow = false;
            continue;
        }
        line.push_back(character);
        // 超长行直接断开：控制协议的单行上限是 4 KiB，这里用更小的可配置
        // 上限，防止一个畸形连接把内存和线程都占住。
        if (line.size() > kAbsoluteMaxControlLineLength
            || line.size() > m_config.m_maxLineLength) {
            overflow = true;
            break;
        }
    }
    if (overflow) {
        writeAll(connection, formatErrorResponse("invalid_line", "line too long",
                                                 contracts::StreamingStatus{}) + "\n");
    }
    closeHandle(connection);
}

std::string ControlServer::handleLine(const std::string &line, const std::string &peerAddress) {
    ControlCommand command;
    std::string errorCode;
    if (!parseControlCommand(line, &command, &errorCode)) {
        return formatErrorResponse(errorCode, "invalid command",
                                   contracts::StreamingStatus{});
    }

    switch (command.m_type) {
        case CommandType::Start: {
            std::string error;
            if (!m_controller.startStreaming(command.m_request, &error)) {
                return formatErrorResponse("start_failed", error,
                                           m_controller.getStreamingStatus(
                                                   command.m_request.m_sessionId));
            }
            const contracts::StreamingStatus status =
                    m_controller.getStreamingStatus(command.m_request.m_sessionId);
            // 密钥只在成功 START 时下发；本阶段没有加密，参数恒为 false 也不影响。
            return formatOkResponse(status, command.m_request.m_encryptionRequested);
        }
        case CommandType::Stop: {
            std::string error;
            if (!m_controller.stopStreaming(command.m_targetSession, &error)) {
                return formatErrorResponse("stop_failed", error,
                                           m_controller.getStreamingStatus(
                                                   command.m_targetSession));
            }
            return formatOkResponse(m_controller.getStreamingStatus(command.m_targetSession));
        }
        case CommandType::Status:
            m_controller.refreshStreamingLease(command.m_targetSession, peerAddress,
                                               command.m_mediaPort);
            return formatOkResponse(m_controller.getStreamingStatus(command.m_targetSession));
        case CommandType::Keyframe: {
            std::string error;
            if (!m_controller.requestKeyframe(command.m_targetSession, &error, nullptr)) {
                return formatErrorResponse("keyframe_failed", error,
                                           m_controller.getStreamingStatus(
                                                   command.m_targetSession));
            }
            return formatOkResponse(m_controller.getStreamingStatus(command.m_targetSession));
        }
        case CommandType::Caps:
            return formatCapsResponse(m_config.m_availableMediaProfiles,
                                      contracts::StreamingStatus{});
    }
    return formatErrorResponse("internal_error", "unhandled command",
                               contracts::StreamingStatus{});
}

}  // namespace control
}  // namespace sserver
