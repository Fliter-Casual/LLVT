// ============================================================
// 文件：LatencyRecorder.cpp
// 模块：sserver::base
// 功能：滑动窗口延迟采样的实现：纳秒样本换算毫秒入窗，容量满淘汰最旧；
//       snapshot 排序后输出 min/avg/p50/p95/p99/max，format 生成单行文案；
//       挂接聚合记录器时样本级联上报。
// 知识点：deque 滑动窗口、lock_guard 临界区最小化、shared_ptr 聚合链、
//         函数模板、线性插值百分位、std::sort / accumulate、iomanip
// ============================================================

#include "sserver/base/LatencyRecorder.h"

#include <algorithm>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <utility>

namespace sserver {
namespace base {

namespace {

constexpr double kNsPerMs = 1000000.0;

// 线性插值百分位：次序统计量位置 = p * (n - 1)，在相邻两个样本之间插值。
// 为什么不用「取第 p*n 个样本」的简化写法：样本少的时候那种算法在 p95 上
// 会直接跳到最大值，n = 2 时 p95 和 p99 都等于 max，看不出尾部差异。
//
// 模板化是因为 deque 和 vector 都只要求 empty / size / operator[]，
// 同一份实现可以直接复用在排序后的任意容器上。
template <typename Container>
double percentileFromSorted(const Container &values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    const double kIndex = percentile * static_cast<double>(values.size() - 1);
    const std::size_t kLower = static_cast<std::size_t>(kIndex);
    // 位置落在最后一个元素上时没有右邻，夹到自身，插值系数自然为 0。
    const std::size_t kUpper = std::min(kLower + 1, values.size() - 1);
    const double kFraction = kIndex - static_cast<double>(kLower);
    return values[kLower] + (values[kUpper] - values[kLower]) * kFraction;
}

}  // namespace

LatencyRecorder::LatencyRecorder(std::size_t maxSamples, std::shared_ptr<LatencyRecorder> aggregate)
        : m_maxSamples(maxSamples), m_aggregate(std::move(aggregate)) {
}

void LatencyRecorder::recordNs(std::uint64_t latencyNs) {
    // 统一按毫秒存储：后续统计与日志都以毫秒为单位，换算只需要做一次。
    const double latencyMs = static_cast<double>(latencyNs) / kNsPerMs;

    if (m_maxSamples > 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (m_samplesMs.size() >= m_maxSamples) {
            m_samplesMs.pop_front();
        }
        m_samplesMs.push_back(latencyMs);
    }

    // 在自身锁之外级联上报：否则会话记录器和全局记录器的锁会嵌套持有，
    // 两个线程反向获取时就死锁了。
    if (m_aggregate) {
        m_aggregate->recordNs(latencyNs);
    }
}

void LatencyRecorder::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_samplesMs.clear();
}

LatencySummary LatencyRecorder::snapshot() const {
    // 锁内只做一次拷贝：排序是 O(n log n)，放在临界区里会把持锁时间
    // 从微秒级拉到毫秒级，而 recordNs 可能被编码线程高频调用。
    std::deque<double> samples;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        samples = m_samplesMs;
    }

    LatencySummary summary;
    summary.m_count = samples.size();
    if (samples.empty()) {
        return summary;
    }

    std::sort(samples.begin(), samples.end());
    summary.m_minMs = samples.front();
    summary.m_maxMs = samples.back();
    // 初值写 0.0（double）而不是 0，否则 accumulate 会按 int 累加把小数截掉。
    summary.m_avgMs = std::accumulate(samples.begin(), samples.end(), 0.0)
            / static_cast<double>(samples.size());
    summary.m_p50Ms = percentileFromSorted(samples, 0.50);
    summary.m_p95Ms = percentileFromSorted(samples, 0.95);
    summary.m_p99Ms = percentileFromSorted(samples, 0.99);
    return summary;
}

std::string LatencyRecorder::format(const std::string &name) const {
    const LatencySummary summary = snapshot();
    std::ostringstream stream;
    // fixed + setprecision(2) 写入后持续生效，后面各字段只需追加值。
    stream << name
           << " count=" << summary.m_count
           << " min=" << std::fixed << std::setprecision(2) << summary.m_minMs << "ms"
           << " avg=" << summary.m_avgMs << "ms"
           << " p50=" << summary.m_p50Ms << "ms"
           << " p95=" << summary.m_p95Ms << "ms"
           << " p99=" << summary.m_p99Ms << "ms"
           << " max=" << summary.m_maxMs << "ms";
    return stream.str();
}

}  // namespace base
}  // namespace sserver
