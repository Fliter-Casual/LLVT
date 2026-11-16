// ============================================================
// 文件：ReceivedFrame.h
// 模块：sclient::transport
// 功能：分片重组完成后交付给播放层的完整帧：分片头元数据（含四个时间戳
//       与关键帧标志）、本地接收时刻、完整码流载荷。
// 知识点：值语义帧结构、时间戳的用途区分（发送端打点 vs 本地接收时刻）
// ============================================================

#ifndef SCLIENT_TRANSPORT_RECEIVED_FRAME_H
#define SCLIENT_TRANSPORT_RECEIVED_FRAME_H

#include <cstdint>
#include <vector>

#include "sclient/transport/protocol/StreamProtocol.h"

namespace sclient {
namespace transport {

struct ReceivedFrame {
    // 首片的头部。帧级字段（序号、四个时间戳、整帧大小、关键帧标志）
    // 各片相同，取首片即可；分片级字段（index/offset/role）不应使用。
    UdpFrameFragmentHeader m_metadata;

    // 本地收到的时刻（单调时钟纳秒）。发送端时间戳属于对端时钟域，
    // 只能用来算"发送端内部"的分段，跨机延迟要靠时钟偏移估计器对齐。
    std::uint64_t m_receiveTimestampNs = 0;

    std::vector<std::uint8_t> m_payload;
};

}  // namespace transport
}  // namespace sclient

#endif  // SCLIENT_TRANSPORT_RECEIVED_FRAME_H
