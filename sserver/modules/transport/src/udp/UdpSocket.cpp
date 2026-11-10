// ============================================================
// 文件：UdpSocket.cpp
// 模块：sserver::transport
// 功能：UDP 套接字的平台实现。系统调用差异集中在本文件，其余模块只看到
//       open/bind/sendTo/recvFrom/close 五个方法。
// 知识点：POSIX 与 winsock 的差异（句柄类型、错误码、超时结构）、
//         一次性的进程级初始化、地址解析
// ============================================================

#include "sserver/transport/udp/UdpSocket.h"

#include <cstring>
#include <mutex>
#include <sstream>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sserver {
namespace transport {

namespace {

#if defined(_WIN32)
using NativeHandle = SOCKET;
constexpr NativeHandle kInvalidHandle = INVALID_SOCKET;
#else
using NativeHandle = int;
constexpr NativeHandle kInvalidHandle = -1;
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

// 超时在两边用不同的结构体表达，语义相同：等待数据的上限。
bool setTimeout(NativeHandle handle, int timeoutMs, std::string *errorMessage) {
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    if (setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&timeout), sizeof(timeout)) != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "failed to set receive timeout, error " + std::to_string(lastSocketError());
        }
        return false;
    }
#else
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    if (setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "failed to set receive timeout, error " + std::to_string(lastSocketError());
        }
        return false;
    }
#endif
    return true;
}

// Windows 要求每个进程先初始化 winsock；用 call_once 保证只做一次，
// 且第一次失败后才不再重试（winsock 初始化失败基本意味着环境有问题）。
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

// 地址字符串转 sockaddr_in。只支持 IPv4：本项目的控制与媒体都在同一
// 局域子网，IPv6 路径没有需求。
bool makeAddress(const std::string &address, int port, sockaddr_in *out, std::string *errorMessage) {
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(static_cast<std::uint16_t>(port));
    const std::string effective = address.empty() ? std::string("0.0.0.0") : address;
    if (inet_pton(AF_INET, effective.c_str(), &out->sin_addr) != 1) {
        if (errorMessage != nullptr) {
            *errorMessage = "invalid IPv4 address '" + effective + "'";
        }
        return false;
    }
    return true;
}

}  // namespace

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open(std::string *errorMessage) {
    if (isOpen()) {
        return true;
    }
    if (!ensurePlatformInitialized(errorMessage)) {
        return false;
    }
    const NativeHandle handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kInvalidHandle) {
        if (errorMessage != nullptr) {
            *errorMessage = "failed to create socket, error " + std::to_string(lastSocketError());
        }
        return false;
    }
    m_handle = wrap(handle);
    return true;
}

bool UdpSocket::bind(const std::string &bindAddress, int port, std::string *errorMessage) {
    if (!isOpen() && !open(errorMessage)) {
        return false;
    }
    sockaddr_in local{};
    if (!makeAddress(bindAddress, port, &local, errorMessage)) {
        return false;
    }
    if (::bind(nativeOf(m_handle), reinterpret_cast<const sockaddr *>(&local),
               static_cast<socklen_t>(sizeof(local))) != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "failed to bind " + bindAddress + ":" + std::to_string(port)
                    + ", error " + std::to_string(lastSocketError());
        }
        return false;
    }
    return true;
}

int UdpSocket::boundPort() const {
    if (!isOpen()) {
        return 0;
    }
    sockaddr_in local{};
    socklen_t length = static_cast<socklen_t>(sizeof(local));
    if (getsockname(nativeOf(m_handle), reinterpret_cast<sockaddr *>(&local), &length) != 0) {
        return 0;
    }
    return static_cast<int>(ntohs(local.sin_port));
}

bool UdpSocket::sendTo(const std::string &address, int port,
                       const std::uint8_t *data, std::size_t size,
                       std::string *errorMessage) {
    if (!isOpen()) {
        if (errorMessage != nullptr) {
            *errorMessage = "socket is not open";
        }
        return false;
    }
    sockaddr_in target{};
    if (!makeAddress(address, port, &target, errorMessage)) {
        return false;
    }
    const int sent = static_cast<int>(::sendto(
            nativeOf(m_handle), reinterpret_cast<const char *>(data), static_cast<int>(size), 0,
            reinterpret_cast<const sockaddr *>(&target), static_cast<socklen_t>(sizeof(target))));
    if (sent < 0 || static_cast<std::size_t>(sent) != size) {
        if (errorMessage != nullptr) {
            *errorMessage = "sendto failed, error " + std::to_string(lastSocketError());
        }
        return false;
    }
    return true;
}

int UdpSocket::recvFrom(std::uint8_t *buffer, std::size_t capacity, int timeoutMs,
                        std::string *peerAddress, int *peerPort, std::string *errorMessage) {
    if (!isOpen()) {
        if (errorMessage != nullptr) {
            *errorMessage = "socket is not open";
        }
        return -1;
    }
    if (!setTimeout(nativeOf(m_handle), timeoutMs, errorMessage)) {
        return -1;
    }
    sockaddr_in peer{};
    socklen_t peerLength = static_cast<socklen_t>(sizeof(peer));
    const int received = static_cast<int>(::recvfrom(
            nativeOf(m_handle), reinterpret_cast<char *>(buffer), static_cast<int>(capacity), 0,
            reinterpret_cast<sockaddr *>(&peer), &peerLength));
    if (received < 0) {
        const int error = lastSocketError();
#if defined(_WIN32)
        const bool timedOut = error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
        const bool timedOut = error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
#endif
        if (timedOut) {
            // 超时不是错误：调用方靠它实现"等到有数据或该干别的了"。
            return 0;
        }
        if (errorMessage != nullptr) {
            *errorMessage = "recvfrom failed, error " + std::to_string(error);
        }
        return -1;
    }
    if (peerAddress != nullptr) {
        char text[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text)) != nullptr) {
            *peerAddress = text;
        }
    }
    if (peerPort != nullptr) {
        *peerPort = static_cast<int>(ntohs(peer.sin_port));
    }
    return received;
}

void UdpSocket::close() {
    if (!isOpen()) {
        return;
    }
    // Linux 上 socket 也要用 close()，和 fd 一样处理。
#if defined(_WIN32)
    ::closesocket(nativeOf(m_handle));
#else
    ::close(nativeOf(m_handle));
#endif
    m_handle = -1;
}

}  // namespace transport
}  // namespace sserver
