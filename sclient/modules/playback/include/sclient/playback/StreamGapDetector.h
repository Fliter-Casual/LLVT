// ============================================================
// 文件：StreamGapDetector.h
// 模块：sclient::playback
// 功能：帧序号连续性检测：判断本帧是否接得上上一帧，统计跨过的帧数。
//       断档意味着参考链已断，解码器必须重同步。
// 知识点：单调序号判定、乱序/重复与真断档的区分、纯逻辑可单测
// ============================================================

#ifndef SCLIENT_PLAYBACK_STREAM_GAP_DETECTOR_H
#define SCLIENT_PLAYBACK_STREAM_GAP_DETECTOR_H

#include <cstdint>

namespace sclient {
namespace playback {

class StreamGapDetector {
public:
    struct Observation {
        bool m_gap = false;              /**< 参考链是否断开 */
        std::uint64_t m_skippedFrames = 0; /**< 向前跳过的帧数；倒退时为 0 */
    };

    // 观察一个完整帧的序号。
    // - 首帧不报警：还没有"上一帧"可以比较
    // - seq == last + 1 连续
    // - seq > last + 1 断档，skipped = seq - last - 1
    // - seq <= last 视为乱序或重复：同样需要重同步（在解码器眼里就是参考
    //   链错位），但不算"跳过"了帧
    Observation observe(std::uint64_t sequence);

    void reset();

    std::uint64_t gapCount() const { return m_gapCount; }
    std::uint64_t skippedFrames() const { return m_skippedFrames; }
    bool hasHistory() const { return m_hasLast; }

private:
    bool m_hasLast = false;
    std::uint64_t m_lastSequence = 0;
    std::uint64_t m_gapCount = 0;
    std::uint64_t m_skippedFrames = 0;
};

}  // namespace playback
}  // namespace sclient

#endif  // SCLIENT_PLAYBACK_STREAM_GAP_DETECTOR_H
