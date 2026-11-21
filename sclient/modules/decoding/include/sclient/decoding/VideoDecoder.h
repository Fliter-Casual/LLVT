// ============================================================
// 文件：VideoDecoder.h
// 模块：sclient::decoding
// 功能：FFmpeg 软解封装：喂入一个完整压缩帧，取出解码后的 YUV 平面；
//       断链后可清空参考状态等待下一个关键帧。
// 知识点：pimpl 隐藏 FFmpeg 头文件、send_packet / receive_frame 拉式接口、
//         平面拷贝与行距保留
// ============================================================

#ifndef SCLIENT_DECODING_VIDEO_DECODER_H
#define SCLIENT_DECODING_VIDEO_DECODER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sclient {
namespace decoding {

// 构建时是否链接了 FFmpeg。上层据此决定是否启用解码，而不是等
// initialize 失败才发现。
bool decodingAvailable();

enum class DecodedPixelFormat {
    Yuv420p,   /**< 三个独立平面 */
    Nv12,      /**< Y 平面 + UV 交织平面 */
    Unsupported,
};

// 解码帧：平面数据已拷贝出来，生命周期与解码器内部缓冲解耦。
// 保留行距是因为 FFmpeg 的帧常有对齐填充，渲染端需要知道真实布局。
struct DecodedFrame {
    int m_width = 0;
    int m_height = 0;
    DecodedPixelFormat m_format = DecodedPixelFormat::Unsupported;
    std::vector<std::uint8_t> m_y;
    std::vector<std::uint8_t> m_u;  /**< Yuv420p 的 U，或 Nv12 的 UV */
    std::vector<std::uint8_t> m_v;  /**< 仅 Yuv420p 使用 */
    int m_yStride = 0;
    int m_uStride = 0;
    int m_vStride = 0;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    // codecName 取控制协议里的取值："h264" / "h265"。
    bool initialize(const std::string &codecName, std::string *errorMessage);

    // 喂一个完整压缩帧（Annex B）。一次调用可能产出 0 帧或多帧
    // （解码器内部有重排序缓冲），调用方把所有帧都收下。
    bool decodePacket(const std::uint8_t *data, std::size_t size,
                      std::vector<DecodedFrame> *frames,
                      std::string *errorMessage);

    // 断链后调用：清空解码器内部缓冲。残留的参考帧已经不可信，
    // 不清掉的话下一个关键帧来了也可能跟旧状态搅在一起解出脏画面。
    void reset();

    void shutdown();

    bool initialized() const;

    // 诊断字段名称稳定、可 grep。
    const std::string &lastError() const;
    int lastFfmpegError() const;
    int decodeErrorCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace decoding
}  // namespace sclient

#endif  // SCLIENT_DECODING_VIDEO_DECODER_H
