// ============================================================
// 文件：UdpSocket.h
// 模块：sserver::transport
// 功能：UDP 数据报套接字的薄封装：创建、绑定、发送、限时接收、关闭。
//       平台上 POSIX 与 winsock 二选一，但公开头文件不出现任何 socket
//       类型与系统宏。
// 知识点：平台抽象、RAII 句柄、SO_RCVTIMEO 限时接收、地址与端口转换
// ============================================================

#ifndef SSERVER_TRANSPORT_UDP_SOCKET_H
#define SSERVER_TRANSPORT_UDP_SOCKET_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace sserver {
namespace transport {

// 句柄存成 intptr_t：POSIX 的 fd 是 int，Windows 的 SOCKET 是 UINT_PTR，
// 两者都能无损放进来，而且无效值在两边都是 -1，调用方不需要分支。
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;

    // 创建数据报套接字。Windows 上顺带完成一次进程级 winsock 初始化。
    bool open(std::string *errorMessage);

    // 绑定到 bindAddress:port。bindAddress 为空或 "0.0.0.0" 表示任意地址；
    // port 为 0 时由内核分配，实际端口用 boundPort() 取。
    bool bind(const std::string &bindAddress, int port, std::string *errorMessage);

    // 未绑定或绑定到 0 端口时，必须在 bind 之后调用才有意义。
    int boundPort() const;

    bool isOpen() const { return m_handle >= 0; }

    bool sendTo(const std::string &address, int port,
                const std::uint8_t *data, std::size_t size,
                std::string *errorMessage);

    // 限时接收一条数据报。
    // @return 收到的字节数；0 表示超时；-1 表示真正的错误
    int recvFrom(std::uint8_t *buffer, std::size_t capacity, int timeoutMs,
                 std::string *peerAddress, int *peerPort, std::string *errorMessage);

    void close();

private:
    std::intptr_t m_handle = -1;
};

}  // namespace transport
}  // namespace sserver

#endif  // SSERVER_TRANSPORT_UDP_SOCKET_H
