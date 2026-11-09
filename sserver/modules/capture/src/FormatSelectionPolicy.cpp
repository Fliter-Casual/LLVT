// ============================================================
// 文件：FormatSelectionPolicy.cpp
// 模块：sserver::capture
// 功能：格式协商纯决策的实现：过滤越界与奇数尺寸、按优先级排序、
//       步进范围向下对齐、去重。
// 知识点：比较器传递性（排序结果稳定）、面积用 64 位避免溢出、
//         std::unique 依赖相邻性
// ============================================================

#include "sserver/capture/FormatSelectionPolicy.h"

#include <algorithm>
#include <cstddef>

namespace sserver {
namespace capture {

namespace {

// 候选尺寸的优先级分值，数字越小越优先。
// 精确请求 > 720p > 其余。720p 单列出来是因为它是"高清但软编跑得动"
// 的现实分界：1080p 软编在低端 ARM 上 p95 会贴着帧间隔。
int sizePriority(const CaptureSize &size,
                 std::uint32_t requestedWidth,
                 std::uint32_t requestedHeight) {
    if (size.m_width == requestedWidth && size.m_height == requestedHeight) {
        return 0;
    }
    if (size.m_width == 1280 && size.m_height == 720) {
        return 1;
    }
    return 2;
}

// 64 位面积：4096×4096 用 32 位算会溢出，虽然实际摄像头到不了这个量级，
// 但比较函数写错时的表现是"排序莫名其妙"，很难查。
std::uint64_t area(const CaptureSize &size) {
    return static_cast<std::uint64_t>(size.m_width) * size.m_height;
}

// 两个候选尺寸是否相同
bool sameSize(const CaptureSize &left, const CaptureSize &right) {
    return left.m_width == right.m_width && left.m_height == right.m_height;
}

}  // namespace

// 候选尺寸是否符合采集请求
bool fitsCaptureRequest(const CaptureSize &size,
                        std::uint32_t maxWidth,
                        std::uint32_t maxHeight) {
    return size.m_width >= 2 && size.m_height >= 2
            && size.m_width <= maxWidth && size.m_height <= maxHeight
            && size.m_width % 2 == 0 && size.m_height % 2 == 0;
}

// 向下对齐到步进的格点
std::uint32_t alignToStep(std::uint32_t desired,
                          std::uint32_t minimum,
                          std::uint32_t maximum,
                          std::uint32_t step) {
    if (minimum > maximum || desired < minimum || step == 0) {
        return 0;
    }
    const std::uint32_t limit = std::min(desired, maximum);
    std::uint32_t value = minimum + (limit - minimum) / step * step;
    if (value % 2 != 0) {
        // 步进本身是偶数时结果不可能为奇数；走到这里说明步进是奇数，
        // 退一个步进是唯一能保持格点的办法，但如果会掉到下限以下就放弃。
        if (step % 2 == 0 || value - minimum < step) {
            return 0;
        }
        value -= step;
    }
    return value;
}

// 优先候选尺寸
std::vector<CaptureSize> preferredCaptureSizes(std::uint32_t requestedWidth,
                                               std::uint32_t requestedHeight) {
    std::vector<CaptureSize> sizes;
    sizes.push_back({requestedWidth, requestedHeight});
    sizes.push_back({1280, 720});
    sizes.push_back({1024, 768});
    sizes.push_back({800, 600});
    sizes.push_back({640, 480});
    sizes.push_back({640, 360});
    sizes.push_back({352, 288});
    sizes.push_back({320, 240});
    sizes.push_back({160, 120});
    return sizes;
}

// 候选尺寸按优先级排序
std::vector<CaptureSize> rankCaptureSizes(const std::vector<CaptureSize> &available,
                                          std::uint32_t requestedWidth,
                                          std::uint32_t requestedHeight) {
    std::vector<CaptureSize> ranked;
    ranked.reserve(available.size());
    for (const CaptureSize &size : available) {
        if (fitsCaptureRequest(size, requestedWidth, requestedHeight)) {
            ranked.push_back(size);
        }
    }
    std::sort(ranked.begin(), ranked.end(),
              [requestedWidth, requestedHeight](const CaptureSize &left,
                                                const CaptureSize &right) {
                  const int leftPriority = sizePriority(left, requestedWidth, requestedHeight);
                  const int rightPriority = sizePriority(right, requestedWidth, requestedHeight);
                  if (leftPriority != rightPriority) {
                      return leftPriority < rightPriority;
                  }
                  const std::uint64_t leftArea = area(left);
                  const std::uint64_t rightArea = area(right);
                  if (leftArea != rightArea) {
                      return leftArea > rightArea;
                  }
                  return left.m_width > right.m_width;
              });
    // unique 只去掉相邻重复项，所以必须在排序之后调用；这里两个同尺寸的
    // 候选在比较器下等价，排序后必然相邻。
    ranked.erase(std::unique(ranked.begin(), ranked.end(), sameSize), ranked.end());
    return ranked;
}

}  // namespace capture
}  // namespace sserver