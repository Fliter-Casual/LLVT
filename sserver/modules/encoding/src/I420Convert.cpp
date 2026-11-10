// ============================================================
// 文件：I420Convert.cpp
// 模块：sserver::encoding
// 功能：YUYV422 → I420 转换的标量实现：亮度直接抽取，色度按 2×2 块
//       垂直平均后水平 2:1 抽取。全程按源行跨距定位，兼容 mmap 填充。
// 知识点：指针算术与行跨距、整数平均的舍入、边界分行处理
// ============================================================

#include "sserver/encoding/I420Convert.h"

namespace sserver {
namespace encoding {

namespace {

// 两个 8 位样本的平均：+1 让结果四舍五入而不是截断，否则整幅画面会
// 系统性地偏暗半个量化级。
std::uint8_t averagePair(std::uint8_t left, std::uint8_t right) {
    return static_cast<std::uint8_t>((static_cast<unsigned int>(left)
                                      + static_cast<unsigned int>(right) + 1U) / 2U);
}

}  // namespace

bool convertYuyv422ToI420(const std::uint8_t *source,
                          std::size_t sourceStride,
                          const I420Planes &destination,
                          int width,
                          int height) {
    if (source == nullptr || destination.m_y == nullptr
        || destination.m_u == nullptr || destination.m_v == nullptr) {
        return false;
    }
    // 4:2:0 的基本约束：宽高必须为偶数，否则色度平面无法均分。
    if (width <= 0 || height <= 0 || width % 2 != 0 || height % 2 != 0) {
        return false;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 2U;
    if (sourceStride < rowBytes
        || destination.m_yStride < static_cast<std::size_t>(width)
        || destination.m_uStride < static_cast<std::size_t>(width / 2)
        || destination.m_vStride < static_cast<std::size_t>(width / 2)) {
        return false;
    }

    const int chromaWidth = width / 2;

    // ---- 亮度平面：每 4 个源字节出两个 Y ----
    for (int row = 0; row < height; ++row) {
        const std::uint8_t *sourceRow = source + static_cast<std::size_t>(row) * sourceStride;
        std::uint8_t *yRow = destination.m_y + static_cast<std::size_t>(row) * destination.m_yStride;
        for (int pair = 0; pair < chromaWidth; ++pair) {
            const std::size_t offset = static_cast<std::size_t>(pair) * 4U;
            yRow[pair * 2] = sourceRow[offset];
            yRow[pair * 2 + 1] = sourceRow[offset + 2];
        }
    }

    // ---- 色度平面：每两行取平均，再按水平 2:1 抽取 ----
    // 宽高在入口已保证为偶数，所以每一行的配对伙伴一定存在。
    for (int row = 0; row < height; row += 2) {
        const std::uint8_t *topRow = source + static_cast<std::size_t>(row) * sourceStride;
        const std::uint8_t *bottomRow =
                source + static_cast<std::size_t>(row + 1) * sourceStride;
        const std::size_t chromaRowIndex = static_cast<std::size_t>(row / 2);
        std::uint8_t *uRow = destination.m_u + chromaRowIndex * destination.m_uStride;
        std::uint8_t *vRow = destination.m_v + chromaRowIndex * destination.m_vStride;

        for (int pair = 0; pair < chromaWidth; ++pair) {
            const std::size_t offset = static_cast<std::size_t>(pair) * 4U;
            uRow[pair] = averagePair(topRow[offset + 1], bottomRow[offset + 1]);
            vRow[pair] = averagePair(topRow[offset + 3], bottomRow[offset + 3]);
        }
    }
    return true;
}

}  // namespace encoding
}  // namespace sserver
