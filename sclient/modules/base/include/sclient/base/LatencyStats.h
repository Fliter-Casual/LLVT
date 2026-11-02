// 客户端延迟统计
// ============================================================
// 文件：LatencyStats.h
// 模块：sclient::base
// 功能：声明环形缓冲区统计器 LatencyStats 与摘要结构 LatencySummary：
//       记录最近 N 个采样，惰性计算 min/avg/p50/p95/p99/max，供抖动缓冲、
//       分段延迟统计等使用。
// 知识点：环形缓冲区、分位数线性插值、mutable 惰性缓存与节流刷新、
//         const 成员函数中的逻辑写、非线程安全类的调用方同步
// ============================================================

#ifndef SCLIENT_BASE_LATENCY_STATS_H
#define SCLIENT_BASE_LATENCY_STATS_H

#include <algorithm>    
#include <cstddef>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace sclient {
namespace base {

// ==================== 统计摘要快照 ====================

// 值类型快照：拿到即脱离统计器，后续采样不会改变已取到的值。
// 各时间字段单位为毫秒。
struct LatencySummary {
    std::size_t m_count = 0;
    double m_lastMs = 0.0;
    double m_minMs = 0.0;
    double m_avgMs = 0.0;
    double m_p50Ms = 0.0;
    double m_p95Ms = 0.0;
    double m_p99Ms = 0.0;
    double m_maxMs = 0.0;
};

// ==================== 环形缓冲区统计器 ====================
// 固定容量 vector + 写入游标，O(1) 覆盖写入；统计窗口永远是「最近 N 个」，
// 而不是全历史——视频链路的网络状况在变，全历史均值没有指导意义。
//
// 惰性刷新：算一次摘要是 O(N log N) 的排序，而查询可能每帧都发生（P95 就是
// 每帧读取的目标延迟输入）。所以结果缓存在 mutable 成员里，每累积
// snapshotRefreshInterval 个新样本才重算一次；format() 会强制刷新，避免
// 日志里出现旧数据。
//
// 线程说明：本类不是线程安全的，调用方需要自行同步（jitter buffer 在
// 单条 I/O 线程上使用，因此不需要内部加锁）。
class LatencyStats {
public:
    explicit LatencyStats(std::size_t maxSamples = 4096, std::size_t snapshotRefreshInterval = 8)
            : m_maxSamples(maxSamples),
              m_snapshotRefreshInterval(std::max<std::size_t>(1, snapshotRefreshInterval)),
              m_samplesMs(maxSamples, 0.0),
              m_nextIndex(0),
              m_sampleCount(0),
              m_dirty(false),
              m_cachedSummaryValid(false),
              m_pendingSnapshotSamples(0),
              m_lastMs(0.0) {
    }

    // maxSamples 为 0 时只更新 lastMs，不保留样本——用于「只要瞬时值、
    // 不要统计」的场景。
    void record(double valueMs) {
        m_lastMs = valueMs;
        if (m_maxSamples == 0) {
            m_dirty = true;
            m_cachedSummaryValid = false;
            m_pendingSnapshotSamples = 0;
            return;
        }

        m_samplesMs[m_nextIndex] = valueMs;
        m_nextIndex = (m_nextIndex + 1) % m_maxSamples;
        // 计数封顶后不再增长，表示「窗口已满、开始覆盖最旧样本」。
        if (m_sampleCount < m_maxSamples) {
            ++m_sampleCount;
        }

        m_dirty = true;
        ++m_pendingSnapshotSamples;
    }

    bool hasSamples() const {
        return m_sampleCount > 0;
    }

    double lastMs() const {
        return m_lastMs;
    }

    double averageMs() const {
        return snapshot().m_avgMs;
    }

    std::size_t sampleCount() const {
        return m_sampleCount;
    }

    // 可能返回上一次缓存的摘要（含旧的 count/lastMs），最多滞后
    // snapshotRefreshInterval 个样本；需要精确值请用 format() 或
    // 接受这里的节流代价。
    LatencySummary snapshot() const {
        refreshSummary(false);
        return m_cachedSummary;
    }

    void reset() {
        // 这里是线性填充：reset 只发生在会话重连等低频路径上。
        std::fill(m_samplesMs.begin(), m_samplesMs.end(), 0.0);
        m_nextIndex = 0;
        m_sampleCount = 0;
        m_dirty = false;
        m_cachedSummaryValid = false;
        m_pendingSnapshotSamples = 0;
        m_lastMs = 0.0;
    }

    // 输出固定格式：name count=X min=Xms avg=Xms p50=Xms p95=Xms p99=Xms max=Xms last=Xms
    std::string format(const std::string &name) const {
        refreshSummary(true);
        const LatencySummary summary = m_cachedSummary;
        std::ostringstream stream;
        stream << name
               << " count=" << summary.m_count
               << " min=" << std::fixed << std::setprecision(2) << summary.m_minMs << "ms"
               << " avg=" << summary.m_avgMs << "ms"
               << " p50=" << summary.m_p50Ms << "ms"
               << " p95=" << summary.m_p95Ms << "ms"
               << " p99=" << summary.m_p99Ms << "ms"
               << " max=" << summary.m_maxMs << "ms"
               << " last=" << summary.m_lastMs << "ms";
        return stream.str();
    }

private:
    // 三级判据：
    //   数据没变且缓存可用        → 直接用
    //   非强制且新增样本不足间隔  → 继续用旧值（节流）
    //   其余                      → 重算
    void refreshSummary(bool forceRefresh) const {
        if (!m_dirty && m_cachedSummaryValid) {
            return;
        }
        if (!forceRefresh &&
            m_cachedSummaryValid &&
            m_pendingSnapshotSamples < m_snapshotRefreshInterval) {
            return;
        }

        m_cachedSummary = buildSummary();
        m_cachedSummaryValid = true;
        m_dirty = false;
        m_pendingSnapshotSamples = 0;
    }

    // 环形缓冲可能已经回绕，逻辑起点是写入游标：先拷 [游标, 末尾) 这段
    // 最旧的，再拷 [开头, 游标) 这段最新的，拼回时间顺序。
    LatencySummary buildSummary() const {
        LatencySummary summary;
        summary.m_count = m_sampleCount;
        summary.m_lastMs = m_lastMs;
        if (m_sampleCount == 0) {
            return summary;
        }

        std::vector<double> orderedSamples(m_sampleCount);
        if (m_sampleCount < m_maxSamples) {
            // 尚未回绕：有效样本就是前 m_sampleCount 个。
            std::copy(
                    m_samplesMs.begin(),
                    m_samplesMs.begin() + static_cast<std::ptrdiff_t>(m_sampleCount),
                    orderedSamples.begin());
        } else {
            const std::size_t kFirstSegmentSize = m_maxSamples - m_nextIndex;
            std::copy(
                    m_samplesMs.begin() + static_cast<std::ptrdiff_t>(m_nextIndex),
                    m_samplesMs.end(),
                    orderedSamples.begin());
            std::copy(
                    m_samplesMs.begin(),
                    m_samplesMs.begin() + static_cast<std::ptrdiff_t>(m_nextIndex),
                    orderedSamples.begin() + static_cast<std::ptrdiff_t>(kFirstSegmentSize));
        }

        std::vector<double> sortedSamples = orderedSamples;
        std::sort(sortedSamples.begin(), sortedSamples.end());
        summary.m_minMs = sortedSamples.front();
        summary.m_maxMs = sortedSamples.back();
        // 均值用未排序序列算，省一次遍历；初值 0.0 锁定 double 累加。
        summary.m_avgMs = std::accumulate(orderedSamples.begin(), orderedSamples.end(), 0.0)
                / static_cast<double>(orderedSamples.size());
        summary.m_p50Ms = percentileFromSorted(sortedSamples, 0.50);
        summary.m_p95Ms = percentileFromSorted(sortedSamples, 0.95);
        summary.m_p99Ms = percentileFromSorted(sortedSamples, 0.99);
        return summary;
    }

    // 线性插值：位置 = p*(n-1)，在相邻两名之间按小数部分插值。
    // 直接向下取整会系统性偏低，向上取整会系统偏高，n 小时阶梯效应明显。
    static double percentileFromSorted(const std::vector<double> &values, double percentile) {
        if (values.empty()) {
            return 0.0;
        }
        const double kIndex = percentile * static_cast<double>(values.size() - 1);
        const std::size_t kLower = static_cast<std::size_t>(kIndex);
        const std::size_t kUpper = std::min(kLower + 1, values.size() - 1);
        const double kFraction = kIndex - static_cast<double>(kLower);
        return values[kLower] + (values[kUpper] - values[kLower]) * kFraction;
    }

    // mutable 是因为 const 查询接口需要刷新缓存；对外可见状态没有改变。
    std::size_t m_maxSamples;
    std::size_t m_snapshotRefreshInterval;
    std::vector<double> m_samplesMs;
    std::size_t m_nextIndex;
    std::size_t m_sampleCount;
    mutable bool m_dirty;
    mutable bool m_cachedSummaryValid;
    mutable std::size_t m_pendingSnapshotSamples;
    mutable LatencySummary m_cachedSummary;
    double m_lastMs;
};

}// namespace base
}// namespace sclient

#endif
