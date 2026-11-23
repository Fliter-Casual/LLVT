// ============================================================
// 文件：StreamGapDetector.cpp
// 模块：sclient::playback
// 功能：断档判定与统计。
// 知识点：溢出安全的比较、状态机式的首次/连续/跳变分支
// ============================================================

#include "sclient/playback/StreamGapDetector.h"

namespace sclient {
namespace playback {

StreamGapDetector::Observation StreamGapDetector::observe(std::uint64_t sequence) {
    Observation observation;
    if (!m_hasLast) {
        // 首帧没有可比较的基准；此时解码器本来就在等第一个关键帧。
        m_hasLast = true;
        m_lastSequence = sequence;
        return observation;
    }

    if (sequence == m_lastSequence + 1) {
        m_lastSequence = sequence;
        return observation;
    }

    observation.m_gap = true;
    ++m_gapCount;
    if (sequence > m_lastSequence + 1) {
        observation.m_skippedFrames = sequence - m_lastSequence - 1;
        m_skippedFrames += observation.m_skippedFrames;
    }
    // 乱序或重复（sequence <= last）：同样要重同步，但不计跳过帧数——
    // 那些帧并没有"丢"，只是来晚了或者重复了。
    m_lastSequence = sequence;
    return observation;
}

void StreamGapDetector::reset() {
    m_hasLast = false;
    m_lastSequence = 0;
    m_gapCount = 0;
    m_skippedFrames = 0;
}

}  // namespace playback
}  // namespace sclient
