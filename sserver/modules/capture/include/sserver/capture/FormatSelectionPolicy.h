// ============================================================
// 文件：FormatSelectionPolicy.h
// 模块：sserver::capture
// 功能：摄像头格式协商中的纯决策部分：候选尺寸过滤、优先级排序、
//       步进范围的向下对齐。不涉及任何 ioctl，因此可以在没有摄像头的
//       机器上完整单测。
// 知识点：偏好排序、整数对齐、去重、策略与 I/O 分离
// ============================================================
// 决定在特定的网络环境和设备条件下，到底该采用哪种“视频格式/参数”来推流。
// ============================================================

#ifndef SSERVER_CAPTURE_FORMAT_SELECTION_POLICY_H
#define SSERVER_CAPTURE_FORMAT_SELECTION_POLICY_H

#include <cstdint>
#include <vector>

namespace sserver {
namespace capture {

// 视频格式/参数的候选尺寸。
struct CaptureSize {
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};

// 4:2:0 编码要求宽高为偶数且至少 2；超过请求上限的尺寸一律不可用——
// 编码器是按上限配置的，超出意味着后续要么缩放要么溢出。
bool fitsCaptureRequest(const CaptureSize &size,
                        std::uint32_t maxWidth,
                        std::uint32_t maxHeight);

// 把期望值向下对齐到 [minimum, maximum] 区间内、步进为 step 的格点上。
// 返回 0 表示该轴上没有可用值：调用方据此丢弃这个候选尺寸，而不是
// 拿着 0 去设置格式。
//
// 为什么是向下对齐：向上取会超过请求上限，而驱动对超大请求的处理方式
// 各不相同（拒绝、截断、静默改成别的尺寸），向下取至少保证结果不会比
// 请求更大。4:2:0 还要求最终值为偶数，若步进是奇数导致结果为奇数、
// 且退一个步进就低于下限，则返回 0。
std::uint32_t alignToStep(std::uint32_t desired,
                          std::uint32_t minimum,
                          std::uint32_t maximum,
                          std::uint32_t step);

// 驱动缺少尺寸枚举接口时的候选清单：请求尺寸打头，其后是常见档位，
// 由调用方逐个尝试 S_FMT。顺序即优先级。
std::vector<CaptureSize> preferredCaptureSizes(std::uint32_t requestedWidth,
                                               std::uint32_t requestedHeight);

// 过滤 + 排序 + 去重，返回按优先级排列的候选尺寸：
//   1. 与请求完全一致的尺寸
//   2. 1280x720（常见的 720p 档，软编 1080p 跑不动时的现实退路）
//   3. 其余按面积从大到小，面积相同则更宽的在前
std::vector<CaptureSize> rankCaptureSizes(const std::vector<CaptureSize> &available,
                                          std::uint32_t requestedWidth,
                                          std::uint32_t requestedHeight);

}  // namespace capture
}  // namespace sserver

#endif  // SSERVER_CAPTURE_FORMAT_SELECTION_POLICY_H
