// ============================================================
// 文件：ControlClient.cpp
// 模块：sclient::control
// 功能：控制客户端的平台实现：非阻塞 connect + select 实现连接超时，
//       SO_RCVTIMEO 实现读取超时，按字节读到换行为止。
// 知识点：非阻塞 connect 的三态（立即成功 / 失败 / 进行中）、
//         select 等待可写判定连接结果、限时逐字节读取
// ============================================================

#include "sclient/control/ControlClient.h"

#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sclient {
namespace control {

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

// 单行响应上限：服务端一条响应最多几百字节，1 KiB 足够，
// 超出视为协议错误而不是继续读——对方可能根本不是我们的服务端。
constexpr std::size_t kMaxResponseBytes = 4096;

NativeHandle nativeOf(std::intptr_t handle) {
    return static_cast<NativeHandle>(handle);
}

int lastSocketError() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

void closeHandle(NativeHandle handle) {
#if defined(_WIN32)
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

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

bool setReceiveTimeout(NativeHandle handle, int timeoutMs, std::string *errorMessage) {
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    if (setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&timeout), sizeof(timeout)) != 0) {
#else
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    if (setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
#endif
        if (errorMessage != nullptr) {
            *errorMessage = "failed to set receive timeout, error "
                    + std::to_string(lastSocketError());
        }
        return false;
    }
    return true;
}

}  // namespace

ControlClient::~ControlClient() {
    close();
}

bool ControlClient::connect(const std::string &host, int port, int timeoutMs,
                            std::string *errorMessage) {
    if (isConnected()) {
        return true;
    }
    if (!ensurePlatformInitialized(errorMessage)) {
        return false;
    }

    const NativeHandle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kInvalidHandle) {
        if (errorMessage != nullptr) {
            *errorMessage = "socket creation failed, error " + std::to_string(lastSocketError());
        }
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        closeHandle(handle);
        if (errorMessage != nullptr) {
            *errorMessage = "invalid control host '" + host + "'";
        }
        return false;
    }

    // 非阻塞 connect：连接超时必须由 select 控制，阻塞 connect 的默认超时
    // 是分钟级的，客户端会长时间卡在"正在连接"上。
#if defined(_WIN32)
    u_long nonBlocking = 1;
    ioctlsocket(handle, FIONBIO, &nonBlocking);
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif

    const int connectResult = ::connect(
            handle, reinterpret_cast<const sockaddr *>(&address),
            static_cast<LengthType>(sizeof(address)));
    if (connectResult != 0) {
#if defined(_WIN32)
        const bool inProgress = lastSocketError() == WSAEWOULDBLOCK;
#else
        const bool inProgress = errno == EINPROGRESS;
#endif
        if (!inProgress) {
            closeHandle(handle);
            if (errorMessage != nullptr) {
                *errorMessage = "connect failed, error " + std::to_string(lastSocketError());
            }
            return false;
        }
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(handle, &writeSet);
        timeval timeout{};
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
        const int ready = ::select(static_cast<int>(handle + 1), nullptr, &writeSet, nullptr,
                                   &timeout);
        if (ready <= 0) {
            closeHandle(handle);
            if (errorMessage != nullptr) {
                *errorMessage = ready == 0 ? "connect timed out" : "connect failed";
            }
            return false;
        }
        int soError = 0;
        LengthType length = static_cast<LengthType>(sizeof(soError));
        getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soError), &length);
        if (soError != 0) {
            closeHandle(handle);
            if (errorMessage != nullptr) {
                *errorMessage = "connect failed, error " + std::to_string(soError);
            }
            return false;
        }
    }

    // 连接建立后恢复阻塞模式：之后的收发都靠 SO_RCVTIMEO 限时。
#if defined(_WIN32)
    u_long blocking = 0;
    ioctlsocket(handle, FIONBIO, &blocking);
#else
    fcntl(handle, F_SETFL, flags);
#endif
    if (!setReceiveTimeout(handle, timeoutMs, errorMessage)) {
        closeHandle(handle);
        return false;
    }

    m_handle = static_cast<std::intptr_t>(handle);
    return true;
}

void ControlClient::close() {
    if (m_handle < 0) {
        return;
    }
    closeHandle(nativeOf(m_handle));
    m_handle = -1;
}

bool ControlClient::execute(const std::string &command, ControlResponse *response,
                            int timeoutMs, std::string *errorMessage) {
    if (!isConnected()) {
        if (errorMessage != nullptr) {
            *errorMessage = "control connection is not open";
        }
        return false;
    }
    if (response == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "execute requires an output response";
        }
        return false;
    }
    if (!setReceiveTimeout(nativeOf(m_handle), timeoutMs, errorMessage)) {
        return false;
    }

    const std::string payload = command + "\n";
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const int sent = static_cast<int>(::send(
                nativeOf(m_handle), payload.data() + offset,
                static_cast<int>(payload.size() - offset), 0));
        if (sent <= 0) {
            if (errorMessage != nullptr) {
                *errorMessage = "send failed, error " + std::to_string(lastSocketError());
            }
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }

    std::string line;
    while (true) {
        char character = 0;
        const int received = static_cast<int>(
                ::recv(nativeOf(m_handle), &character, 1, 0));
        if (received <= 0) {
            if (errorMessage != nullptr) {
                *errorMessage = received == 0
                        ? "control connection closed by peer"
                        : "receive failed, error " + std::to_string(lastSocketError());
            }
            return false;
        }
        if (character == '\n') {
            break;
        }
        if (character != '\r') {
            line.push_back(character);
        }
        if (line.size() > kMaxResponseBytes) {
            if (errorMessage != nullptr) {
                *errorMessage = "control response exceeds the line limit";
            }
            return false;
        }
    }

    ControlResponse parsed = parseControlResponse(line);
    // 首词不是 OK/ERR：说明对端不是我们的服务端（或被中间设备改了内容），
    // 这属于协议错误，不能当成一次"业务失败"继续跑。
    if (!parsed.m_ok && parsed.m_errorCode.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "unrecognized control response: " + line.substr(0, 64);
        }
        return false;
    }
    *response = std::move(parsed);
    return true;
}

}  // namespace control
}  // namespace sclient
