// ============================================================
// 文件：MonotonicClock.h
// 模块：sclient::base（与服务端同款，两端日志与统计的时间源一致）
// 功能：声明 monotonicNowNs()：返回 steady_clock 距自身纪元的纳秒计数，
//       作为本进程超时、发送节奏与延迟统计的统一时间源。
// 知识点：单调时钟与挂钟时钟的区别、chrono 时长转换、header-only 内联函数
// ============================================================

#ifndef SCLIENT_BASE_MONOTONIC_CLOCK_H
#define SCLIENT_BASE_MONOTONIC_CLOCK_H

#include <chrono>
#include <cstdint>

namespace sclient {
namespace base {

// 为什么不用 system_clock：挂钟会被 NTP 校正或手动改时间，跳变一次就会
// 算出负的时间间隔，或者让 30 秒租约瞬间过期。steady_clock 只保证单调
// 递增，正好是「测量间隔」需要的语义。
//
// 代价是它的纪元是实现定义的（Linux 上通常是开机以来的时间），既不等同
// 于 Unix 时间，也不能与另一台主机的计数直接相减——跨主机的时间对齐是
// 客户端时钟偏移估计器的事，这里只解决单进程内的打点。
inline std::uint64_t monotonicNowNs() {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
}

}  // namespace base
}  // namespace sclient

#endif  // SCLIENT_BASE_MONOTONIC_CLOCK_H
