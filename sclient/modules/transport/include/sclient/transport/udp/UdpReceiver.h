// ============================================================
// 文件：UdpReceiver.h
// 模块：sclient::transport
// 功能：媒体接收端：在绑定端口上收数据报、校验并解析两类头部、交给组装器；
//       对外只暴露"收一帧"一个同步方法。
// 知识点：数据报校验顺序（魔数 → 类型 → 长度）、部分读取、超时预算分摊
// ============================================================

#ifndef SCLIENT_TRANSPORT_UDP_RECEIVER_H
#define SCLIENT_TRANSPORT_UDP_RECEIVER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sclient/transport/ReceivedFrame.h"
#include "sclient/transport/udp/UdpFrameAssembler.h"
#include "sclient/transport/udp/UdpSocket.h"

namespace sclient {
namespace transport {

class UdpReceiver {
public:
    explicit UdpReceiver(const UdpFrameAssembler::Options &options = UdpFrameAssembler::Options());

    bool open(const std::string &bindAddress, int port, std::string *errorMessage);
    int boundPort() const { return m_socket.boundPort(); }
    void close() { m_socket.close(); }

    // 收一帧。内部循环接收数据报直到拼出一整帧，或超时用完。
    // @return 成功拼出一帧返回 true；超时返回 false（不是错误）；
    //         errorMessage 非空且返回 false 时可能带有真正的错误原因
    bool receiveFrame(ReceivedFrame *frame, int timeoutMs, std::string *errorMessage);

    // 在同一个套接字上发数据报。注册报告、保活、NACK 都必须从收流的这个
    // 端口发出：服务端从报告数据报的源地址学习接收端，换了端口就收不到帧。
    bool sendDatagram(const std::string &address, int port,
                      const std::uint8_t *data, std::size_t size,
                      std::string *errorMessage) {
        return m_socket.sendTo(address, port, data, size, errorMessage);
    }

    UdpFrameAssembler::Stats stats() const { return m_assembler.stats(); }

private:
    UdpSocket m_socket;
    UdpFrameAssembler m_assembler;
    std::vector<std::uint8_t> m_buffer;
};

}  // namespace transport
}  // namespace sclient

#endif  // SCLIENT_TRANSPORT_UDP_RECEIVER_H
