// ============================================================
// 文件：LatencyRecorder.h
// 模块：sserver::base
// 功能：声明滑动窗口延迟采样器 LatencyRecorder 与摘要结构 LatencySummary。
//       各流水线阶段以纳秒记录样本，按毫秒统计 p50/p95/p99，并支持向上
//       聚合成全局视图，供性能摘要日志与端到端统计使用。
// 知识点：滑动窗口统计、线性插值分位数、纳秒/毫秒换算、
//         mutex 保护的 deque、级联聚合（会话 → 全局）
// ============================================================

#ifndef SSERVER_BASE_LATENCY_RECORDER_H
#define SSERVER_BASE_LATENCY_RECORDER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace sserver {
namespace base {

// ==================== 延迟摘要 ====================

// 时间字段单位为毫秒；m_count 为窗口内样本数，分位数由 snapshot() 对
// 窗口样本排序后按位置取值。count 为 0 时其余字段保持 0——调用方靠
// count 判断「没有数据」，不要靠 0.0 去猜。
struct LatencySummary {
    std::size_t m_count = 0; // 样本数
    double m_minMs = 0.0;
    double m_avgMs = 0.0;
    double m_p50Ms = 0.0;
    double m_p95Ms = 0.0;
    double m_p99Ms = 0.0;
    double m_maxMs = 0.0;
};

// ==================== 延迟采样器 ====================

// 线程安全的滑动窗口采样器：recordNs 记录一条样本（内部换算毫秒，容量满
// 时淘汰最旧），snapshot 排序后算出分位数摘要。
//
// 为什么要 p95 而不只看平均值：视频链路的延迟分布是长尾的，平均值会把
// 偶尔卡住的那几百毫秒摊平掉，而用户感知到的卡顿恰恰来自尾部。
//
// 聚合：构造时传入 aggregate，每条样本会同步累加到上级记录器，形成
// 「会话 → 全局」的树。聚合调用放在本地锁之外，两个记录器的锁不会嵌套。
// maxSamples 为 0 表示不保留本地样本、只向上上报，用于纯转发节点。
class LatencyRecorder {
public:
    // 禁止编译器进行“隐式类型转换”
    explicit LatencyRecorder(std::size_t maxSamples = 4096,
                             std::shared_ptr<LatencyRecorder> aggregate = nullptr);
    ~LatencyRecorder() = default;

    LatencyRecorder(const LatencyRecorder &) = delete;
    LatencyRecorder &operator=(const LatencyRecorder &) = delete;

    void recordNs(std::uint64_t latencyNs);

    // 只清空自身窗口，不递归清空聚合方：聚合方的样本来自多个会话，
    // 单个会话复位不应该把别人的数据一起抹掉。
    void reset();

    LatencySummary snapshot() const;

    // 生成单行统计文案，可直接拼进结构化日志的字段里。
    std::string format(const std::string &name) const;

private:
    // 容量与聚合目标在构造后不再改变。
    const std::size_t m_maxSamples;
    // 聚合目标:汇总了所有会话的数据。只要调它的 snapshot()，就能直接拿到整个服务器的总延迟指标。
    const std::shared_ptr<LatencyRecorder> m_aggregate; // 聚合目标
    mutable std::mutex m_mutex; // 保护 m_samplesMs 的锁
    std::deque<double> m_samplesMs; // 样本队列
};

} // namespace base
} // namespace sserver


#endif