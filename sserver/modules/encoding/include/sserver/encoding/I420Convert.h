// ============================================================
// 文件：I420Convert.h
// 模块：sserver::encoding
// 功能：声明 YUYV422 → I420 的色彩格式转换，供 x264 软编路径使用。
//       支持源缓冲行跨距（零拷贝 mmap 视图的行尾可能有填充）。
// 知识点：4:2:2 到 4:2:0 的色度垂直下采样、行跨距感知、纯函数便于单测
// ============================================================

#ifndef SSERVER_ENCODING_I420_CONVERT_H
#define SSERVER_ENCODING_I420_CONVERT_H

#include <cstddef>
#include <cstdint>

namespace sserver {
namespace encoding {

// I420 三平面的输出目标。行距单独给出：编码器的暂存平面可能按
// 16 或 32 字节对齐分配，不能假定等于宽（亮度）或宽/2（色度）。
struct I420Planes {
    std::uint8_t *m_y = nullptr;
    std::uint8_t *m_u = nullptr;
    std::uint8_t *m_v = nullptr;
    std::size_t m_yStride = 0;
    std::size_t m_uStride = 0;
    std::size_t m_vStride = 0;
};

// YUYV422 每两个相邻像素占 4 字节：Y0 U Y1 V。色度水平方向已是 2:1，
// 转 4:2:0 还需要在垂直方向再降一次采样。
//
// 本实现取上下两行色度的平均值（而不是只取偶数行）：只取一行等于最近邻
// 下采样，垂直方向会出现色度锯齿，红色边缘尤其明显；平均的代价只是每对
// 多两次加法。
//
// 参数不合法（空指针、宽高非正或非偶数、行距不足）时返回 false 且不写入
// 任何字节——4:2:0 要求宽高均为偶数，这是调用方必须在协商阶段保证的。
bool convertYuyv422ToI420(const std::uint8_t *source,
                          std::size_t sourceStride,
                          const I420Planes &destination,
                          int width,
                          int height);

}  // namespace encoding
}  // namespace sserver

#endif  // SSERVER_ENCODING_I420_CONVERT_H
