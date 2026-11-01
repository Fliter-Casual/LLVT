// ============================================================
// 文件：JitterBufferTypes.h
// 模块：sclient::transport
// 功能：声明抖动缓冲的公共类型：缓冲模式 JitterBufferMode 与网络质量评级
//       NetworkQuality，以及两者的日志名映射；供自适应控制器与统计快照
//       （UdpReceiveStats）共用。
// 知识点：enum class 强类型枚举、inline 名称映射返回静态字符串（零分配）
// ============================================================

#ifndef SCLIENT_TRANSPORT_JITTER_BUFFER_TYPES_H
#define SCLIENT_TRANSPORT_JITTER_BUFFER_TYPES_H

namespace sclient {
namespace transport {

// ==================== 模式与质量评级 ====================

// 观测与策略分离：NetworkQuality 是对网络的「观测」，JitterBufferMode 是
// 对缓冲的「策略」。质量档位决定等待上限与帧数上限，模式只决定目标延迟，
// 两者由不同的判据驱动，混在一起会让调参无从下手。
// 策略
enum class JitterBufferMode {
    Bypass,      /**< 旁路：目标缓冲延迟为零，缺帧等待与调度仍可能等待 */
    LowLatency,  /**< 低延迟：最小缓冲，适合稳定网络 */
    Smooth,      /**< 流畅：较大缓冲，适合抖动明显的网络 */
};

// 观测
enum class NetworkQuality {
    Excellent,  /**< 各项指标在最优阈值内 */
    Good,       /**< 轻微抖动 */
    Fair,       /**< 中度抖动、丢包或跳帧 */
    Poor,       /**< 超过最差阈值，需要激进保护 */
};

// ==================== 日志名映射 ====================

// 返回静态字符串：热路径每帧都可能格式化，不能有分配。
inline const char *jitterBufferModeName(JitterBufferMode mode) {
    switch (mode) {
        case JitterBufferMode::Bypass: return "bypass";
        case JitterBufferMode::LowLatency: return "low_latency";
        case JitterBufferMode::Smooth: return "smooth";
    }
    return "unknown";
}

inline const char *networkQualityName(NetworkQuality quality) {
    switch (quality) {
        case NetworkQuality::Excellent: return "excellent";
        case NetworkQuality::Good: return "good";
        case NetworkQuality::Fair: return "fair";
        case NetworkQuality::Poor: return "poor";
    }
    return "unknown";
}

} // namespace sclient
} // namespace transport

#endif