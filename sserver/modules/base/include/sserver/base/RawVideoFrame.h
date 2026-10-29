// ============================================================
// 文件：RawVideoFrame.h
// 模块：sserver::base
// 功能：声明原始视频帧 RawVideoFrame、其只读视图 RawVideoFrameView 与
//       像素格式枚举。这是采集到编码之间传递未压缩帧的基本单元，
//       支持三种数据形态：自有字节拷贝、mmap 零拷贝引用、DMA-BUF 句柄。
// 涵盖：视图（不持有数据）、RAII 租约(带有时间限制的授权)与释放回调、不可拷贝类型、
//         shared_ptr 共享所有权、YUYV422 / MJPEG、DMA-BUF
// ============================================================

#ifndef SSERVER_BASE_RAW_VIDEO_FRAME_H
#define SSERVER_BASE_RAW_VIDEO_FRAME_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace sserver {
namespace base { 

// ==================== 像素格式 ====================

// Unknown 用于默认构造或格式协商失败的场景；下游必须显式拒绝。
enum class RawVideoPixelFormat {
    Unknown,
    Yuyv422,
    Mjpeg,
}; //  USB 摄像头（UVC 设备）采集输出的原始格式

// ==================== 只读视图 ====================

// 只记录指针、长度与几何信息的值类型：消费方凭它读帧，但不负责释放
// 底层缓冲，也不延长帧租约的生命周期。CPU 指针与 DMA-BUF 句柄可以同时
// 有效，由消费方按自己支持的通路择一使用。
//
// m_stride 是紧凑格式的字节行距：MMAP 映射出来的画面行尾可能带填充，
// 跨度不等于「宽度乘每像素字节数」，按紧凑排列去读会得到歪斜的画面。
// MJPEG 是自描述的压缩流，保持为 0。
struct RawVideoFrameView {
    // 视频像素格式（如 YUV420P、NV12、RGB 等），默认初始化为未知
    RawVideoPixelFormat m_pixelFormat = RawVideoPixelFormat::Unknown;
    // 指向实际视频帧数据的只读指针（通常是 CPU 映射(mmap)后的虚拟地址）
    const std::uint8_t *m_data = nullptr; 
    // 当前帧实际使用的字节数（注意：可能小于 m_dmaBufSize）
    std::size_t m_bytesUsed = 0;
    // DMA-BUF 的文件描述符（句柄），-1 表示未绑定硬件缓冲区（说明是纯 CPU 内存）
    int m_dmaBufFd = -1;
    // DMA-BUF 映射的物理/虚拟内存总大小
    std::size_t m_dmaBufSize = 0;
    // 视频帧的宽度（像素）
    int m_width = 0;
    // 视频帧的高度（像素）
    int m_height = 0;
    // 步长（Stride/Pitch），即每行像素所占的字节数（因内存对齐，通常大于等于 宽度 * 每像素字节数）
    std::size_t m_stride = 0;
};

// ==================== 帧本体 ====================

// 持有拷贝的 CPU 字节，或一个 V4L2 采集缓冲的租约(带有时间限制的授权)。
//
// 租约模式下 m_releaseCallback 把已出队的缓冲归还驱动；帧在构成后不可
// 拷贝（拷贝会让两个对象都持有同一个归还钩子，析构时重复归还），跨线程
// 流转一律经 RawVideoFramePtr 共享。移动是允许的：所有权与归还钩子一起
// 转移，被移走的一方不再触发回调。
struct RawVideoFrame {
    RawVideoFrame() = default;
    ~RawVideoFrame() {
        if (m_releaseCallback) {
            m_releaseCallback();
        }
    }

    RawVideoFrame(const RawVideoFrame &) = delete;
    RawVideoFrame &operator=(const RawVideoFrame &) = delete;

    // 移动构造函数， noexcept:说服容器不抛异常，进行高效移动;否则中途抛异常它会变成拷贝
    RawVideoFrame(RawVideoFrame &&other) noexcept 
        : m_data(std::move(other.m_data)),
          m_externalData(other.m_externalData),
          m_captureTimestampNs(other.m_captureTimestampNs),
          m_bytesUsed(other.m_bytesUsed),
          m_pixelFormat(other.m_pixelFormat),
          m_dmaBufFd(other.m_dmaBufFd),
          m_dmaBufSize(other.m_dmaBufSize),
          m_width(other.m_width),
          m_height(other.m_height),
          m_stride(other.m_stride),
          m_releaseCallback(std::move(other.m_releaseCallback)) {
        other.m_releaseCallback = nullptr;
        other.m_externalData = nullptr;
        other.m_bytesUsed = 0;
    }

    RawVideoFrame &operator=(RawVideoFrame &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        // 先归还自己持有的租约，再接管对方的，避免缓冲泄漏。
        if (m_releaseCallback) {
            m_releaseCallback();
        }
        m_data = std::move(other.m_data);
        m_externalData = other.m_externalData;
        m_captureTimestampNs = other.m_captureTimestampNs;
        m_bytesUsed = other.m_bytesUsed;
        m_pixelFormat = other.m_pixelFormat;
        m_dmaBufFd = other.m_dmaBufFd;
        m_dmaBufSize = other.m_dmaBufSize;
        m_width = other.m_width;
        m_height = other.m_height;
        m_stride = other.m_stride;
        m_releaseCallback = std::move(other.m_releaseCallback);
        other.m_releaseCallback = nullptr;
        other.m_externalData = nullptr;
        other.m_bytesUsed = 0;
        return *this;
    }

    // 视图不延长帧的生命周期：调用方必须保证在读取视图期间本帧仍然存活。
    RawVideoFrameView view() const {
        RawVideoFrameView result;
        result.m_pixelFormat = m_pixelFormat;
        result.m_data = m_externalData != nullptr
                ? m_externalData
                : (m_data.empty() ? nullptr : m_data.data());
        result.m_bytesUsed = m_bytesUsed;
        result.m_dmaBufFd = m_dmaBufFd;
        result.m_dmaBufSize = m_dmaBufSize;
        result.m_width = m_width;
        result.m_height = m_height;
        result.m_stride = m_stride;
        return result;
    }

    std::vector<std::uint8_t> m_data;                /**< 自有拷贝，MMAP 复制路径使用 */
    const std::uint8_t *m_externalData = nullptr;    /**< 零拷贝引用 V4L2 mmap 缓冲，无所有权，生命周期由 m_releaseCallback 保证 */
    std::uint64_t m_captureTimestampNs = 0;          /**< 采集打点（单调时钟纳秒） */
    std::size_t m_bytesUsed = 0;                     /**< m_data 或外部缓冲的实际字节数 */
    RawVideoPixelFormat m_pixelFormat = RawVideoPixelFormat::Unknown;
    int m_dmaBufFd = -1;                             /**< DMA-BUF 文件描述符，-1 表示无 */
    std::size_t m_dmaBufSize = 0;
    int m_width = 0;
    int m_height = 0;
    std::size_t m_stride = 0;
    std::function<void()> m_releaseCallback;         /**< 析构时归还 V4L2 缓冲；空表示无需归还 */
};

// 帧跨线程、跨队列流转的标准载体：多个消费者共享同一帧，最后一个引用
// 释放时才析构并归还缓冲。
using RawVideoFramePtr = std::shared_ptr<RawVideoFrame>;

}  // namespace base
}  // namespace sserver

#endif  // SSERVER_BASE_RAW_VIDEO_FRAME_H