// ============================================================
// 文件：UdpSender.h
// 模块：sserver::transport
// 功能：把一个编码帧切成若干 UDP 数据报发往目标端点：逐片写帧头与分片头，
//       可选令牌桶整形。不持有线程，节奏由调用方（发送线程）驱动。
// 知识点：分片与偏移、线格式拼装、可选整形、发送结果的计数回传
// ============================================================

#ifndef SSERVER_TRANSPORT_UDP_SENDER_H
#define SSERVER_TRANSPORT_UDP_SENDER_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "sserver/base/EncodedFrame.h"
#include "sserver/transport/udp/TokenBucket.h"
#include "sserver/transport/udp/UdpSocket.h"

namespace sserver {
namespace transport {

struct UdpSenderConfig {
    std::size_t m_fragmentPayloadBytes = 1200; /**< 每片携带的码流字节数 */

    // 整形参数。m_pacingBytesPerSecond <= 0 表示关闭整形（测试与局域回环用）。
    double m_pacingBytesPerSecond = 0.0;
    double m_pacingBurstBytes = 0.0;

    // 单帧最多为等额度睡多久。超过就照常发出去——整形是为了削峰，
    // 不是为了把整个关键帧丢掉；丢一帧会让整条参考链断掉。
    std::uint64_t m_pacingMaxWaitNs = 200ULL * 1000ULL * 1000ULL;
};

class UdpSender {
public:
    struct SendResult {
        std::size_t m_datagrams = 0;
        std::size_t m_bytes = 0;      /**< 含头部在内的总字节数 */
        bool m_paced = false;         /**< 是否因为等额度推迟过发送 */
    };

    // 分片大小在发送前确定：构造后改它会让同一帧的分片大小不一致。
    explicit UdpSender(const UdpSenderConfig &config = UdpSenderConfig());

    bool open(const std::string &bindAddress, int port, std::string *errorMessage);

    int boundPort() const { return m_socket.boundPort(); }

    void close() { m_socket.close(); }

    // 把一帧切成 ceil(payload / fragmentSize) 个数据报发出。
    //
    // @param transportFrameIndex 进入发送路径的帧序号，写进分片头供接收端
    //        区分"同一编码序号的不同传输实例"
    // @param sendTimestampNs 发送时刻，同一帧的各分片共用
    bool sendFrame(const base::EncodedFrame &frame,
                   const std::string &targetAddress,
                   int targetPort,
                   std::uint64_t transportFrameIndex,
                   std::uint64_t sendTimestampNs,
                   SendResult *result,
                   std::string *errorMessage);

    // 在同一个套接字上收一条反馈数据报。服务端不需要给每个客户端单独开
    // 接收端口：客户端把报告发到这个媒体端口，服务端从合法报告里学习
    // 对端地址，也就顺带完成了地址注册。
    // @return 字节数；0 表示超时；-1 表示错误
    int receiveFeedback(std::uint8_t *buffer, std::size_t capacity, int timeoutMs,
                        std::string *peerAddress, int *peerPort, std::string *errorMessage);

private:
    UdpSenderConfig m_config;
    UdpSocket m_socket;
    TokenBucket m_bucket;
    std::uint64_t m_nextPacketSequence = 0;
};

}  // namespace transport
}  // namespace sserver

#endif  // SSERVER_TRANSPORT_UDP_SENDER_H
