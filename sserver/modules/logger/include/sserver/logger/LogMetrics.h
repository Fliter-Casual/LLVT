// ============================================================
// 文件：LogMetrics.h
// 模块：sserver::logger
// 功能：指标日志的格式化辅助：metricValue 统一浮点文本精度，
//       appendLatencyFields 把延迟统计摘要展开为 logfmt 键值字段，
//       供各模块把性能数据写进结构化事件日志。
// 知识点：header-only 内联工具、流式格式化、百分位指标命名约定
// ============================================================
// 把生涩难懂的 C++ 数据结构（延迟统计摘要），转换为便于人类阅读和机器解析的格式化日志字符串
#ifndef SSERVER_LOGGER_LOG_METRICS_H
#define SSERVER_LOGGER_LOG_METRICS_H

#include <iomanip>
#include <sstream>
#include <string>

#include "sserver/base/LatencyRecorder.h"
#include "sserver/logger/Logger.h"

namespace sserver {
namespace logger {

// 固定三位小数：指标值如果进日志时位数不定，同一条 grep 出来的结果
// 就没法直接做数值比较，脚本还得先做格式化。
inline std::string metricValue(double value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(3) << value;
    return output.str();
}

// 把延迟摘要展开为 logfmt 字段：键名统一为 <prefix>_<指标>，单位后缀
// 直接写进键名（_ms），这样日志消费方不需要知道单位约定。
inline void appendLatencyFields(const std::string &prefix,
                                const base::LatencySummary &summary,
                                LogFields *fields) {
    if (fields == nullptr) {
        return;
    }
    fields->push_back({prefix + "_samples", std::to_string(summary.m_count)});
    fields->push_back({prefix + "_min_ms", metricValue(summary.m_minMs)});
    fields->push_back({prefix + "_avg_ms", metricValue(summary.m_avgMs)});
    fields->push_back({prefix + "_p50_ms", metricValue(summary.m_p50Ms)});
    fields->push_back({prefix + "_p95_ms", metricValue(summary.m_p95Ms)});
    fields->push_back({prefix + "_p99_ms", metricValue(summary.m_p99Ms)});
    fields->push_back({prefix + "_max_ms", metricValue(summary.m_maxMs)});
}

}  // namespace logger
}  // namespace sserver

#endif  // SSERVER_LOGGER_LOG_METRICS_H
