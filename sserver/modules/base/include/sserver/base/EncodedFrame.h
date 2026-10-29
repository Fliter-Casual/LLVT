// ============================================================
// 文件：EncodedFrame.h
// 模块：sserver::base
// 功能：声明编码后的单帧数据 EncodedFrame 与流载荷类型枚举。它是编码器
//       输出、传输层发送的基本交换单元，帧内携带采集与编码各阶段的
//       时间戳，供端到端延迟统计与客户端诊断使用。
// 知识点：enum class 指定底层类型、单调时钟纳秒打点、关键帧标志、
//         shared_ptr<const T> 只读共享
// ============================================================

#ifndef SSERVER_BASE_ENCODED_FRAME_H
#define SSERVER_BASE_ENCODED_FRAME_H

#include <cstdint>
#include <memory>
#include <vector>

#include "sserver/base/VideoCodec.h"

namespace sserver {
namespace base {

// ==================== 流载荷类型 ====================

// 数值直接写入 UDP 消息头的 subType，与客户端镜像定义必须一致，
// 所以底层类型定死为 uint16_t 并按小端序列化。当前只产生 Video。
enum class StreamPayloadType : std::uint16_t {
    Video = 3,
};

// ==================== 编码帧 ====================

// 传输层打点：(在网络数据发送和接收的环节，记录下精确的时间，用来测量这一段的耗时)
// 值语义结构：m_payload 随结构体整体移动；入队后内容不再修改，经
// EncodedFramePtr 以 const 形式共享给多个消费者，读取无需加锁。
//
// 时间戳都是单调时钟纳秒，与墙钟无关、不受系统改时间影响。四个时间戳里
// 这里只盖三个：发送时刻由传输层在真正把包交给内核时盖上，写进线上帧头。
struct EncodedFrame {
    StreamPayloadType m_type = StreamPayloadType::Video; // 帧类型，用于传输层打点
    std::uint64_t m_sequence = 0;   /**< 编码子流内成功编码帧的递增序号；跨子流可重复，须结合会话/档位关联 */
    std::uint64_t m_encodingId = 0; /**< 编码子流实例标识，供传输区分「按需重建」与「同一流内丢帧」 */
    std::uint64_t m_captureTimestampNs = 0;     // 采集时刻，用于传输层打点
    std::uint64_t m_encodeStartTimestampNs = 0; // 编码开始时刻，用于传输层打点
    std::uint64_t m_encodeEndTimestampNs = 0;   // 编码结束时刻，用于传输层打点
    bool m_isKeyframe = false;  /**< 是否关键帧 */
    // 本帧实际编码，供载荷 NAL 解析与诊断选择语义；默认 H.264 兼容软编路径。
    codec::VideoCodec m_videoCodec = codec::VideoCodec::kH264;
    std::vector<std::uint8_t> m_payload; /**< Annex B 码流，含参数集与起始码 */
};

// 帧内容不可变，多个消费者并发读取无需加锁。
using EncodedFramePtr = std::shared_ptr<const EncodedFrame>;

}  // namespace base
}  // namespace sserver

#endif  // SSERVER_BASE_ENCODED_FRAME_H