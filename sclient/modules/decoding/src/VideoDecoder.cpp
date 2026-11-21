// ============================================================
// 文件：VideoDecoder.cpp
// 模块：sclient::decoding
// 功能：FFmpeg 软解实现。未链接 FFmpeg 时提供明确失败的实现。
// 知识点：avcodec 现代 send/receive 接口、EAGAIN 语义、AVFrame 平面拷贝、
//         avcodec_flush_buffers 断链重置
// ============================================================

#include "sclient/decoding/VideoDecoder.h"

#include <cstring>
#include <utility>

#if defined(SCLIENT_HAS_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
}
#endif

namespace sclient {
namespace decoding {

#if defined(SCLIENT_HAS_FFMPEG)

namespace {

std::string ffmpegErrorText(int code) {
    char buffer[256] = {0};
    if (av_strerror(code, buffer, sizeof(buffer)) < 0) {
        return "ffmpeg error " + std::to_string(code);
    }
    return buffer;
}

// 把一个 AVFrame 的平面拷进 DecodedFrame。保留源行距：FFmpeg 的帧通常
// 按 16/32 字节对齐，行尾有填充，渲染端上传纹理时必须按真实行距走。
bool copyFrame(AVFrame *frame, DecodedFrame *out, std::string *errorMessage) {
    if (frame->width <= 0 || frame->height <= 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "decoded frame has invalid dimensions";
        }
        return false;
    }
    out->m_width = frame->width;
    out->m_height = frame->height;

    const int width = frame->width;
    const int height = frame->height;

    if (frame->format == AV_PIX_FMT_YUV420P) {
        out->m_format = DecodedPixelFormat::Yuv420p;
        out->m_y.resize(static_cast<std::size_t>(width) * height);
        out->m_u.resize(static_cast<std::size_t>(width / 2) * (height / 2));
        out->m_v.resize(static_cast<std::size_t>(width / 2) * (height / 2));
        // 逐行拷贝而不是整块拷：源行距可能大于宽度，整块拷会把填充字节
        // 也带进紧凑目标，画面会从第二行开始歪斜。
        for (int row = 0; row < height; ++row) {
            std::memcpy(out->m_y.data() + static_cast<std::size_t>(row) * width,
                        frame->data[0] + static_cast<std::size_t>(row) * frame->linesize[0],
                        static_cast<std::size_t>(width));
        }
        for (int row = 0; row < height / 2; ++row) {
            std::memcpy(out->m_u.data() + static_cast<std::size_t>(row) * (width / 2),
                        frame->data[1] + static_cast<std::size_t>(row) * frame->linesize[1],
                        static_cast<std::size_t>(width / 2));
            std::memcpy(out->m_v.data() + static_cast<std::size_t>(row) * (width / 2),
                        frame->data[2] + static_cast<std::size_t>(row) * frame->linesize[2],
                        static_cast<std::size_t>(width / 2));
        }
        out->m_yStride = width;
        out->m_uStride = width / 2;
        out->m_vStride = width / 2;
        return true;
    }

    if (frame->format == AV_PIX_FMT_NV12) {
        out->m_format = DecodedPixelFormat::Nv12;
        out->m_y.resize(static_cast<std::size_t>(width) * height);
        out->m_u.resize(static_cast<std::size_t>(width) * (height / 2));
        out->m_v.clear();
        for (int row = 0; row < height; ++row) {
            std::memcpy(out->m_y.data() + static_cast<std::size_t>(row) * width,
                        frame->data[0] + static_cast<std::size_t>(row) * frame->linesize[0],
                        static_cast<std::size_t>(width));
        }
        for (int row = 0; row < height / 2; ++row) {
            std::memcpy(out->m_u.data() + static_cast<std::size_t>(row) * width,
                        frame->data[1] + static_cast<std::size_t>(row) * frame->linesize[1],
                        static_cast<std::size_t>(width));
        }
        out->m_yStride = width;
        out->m_uStride = width;
        out->m_vStride = 0;
        return true;
    }

    // 其他格式（10bit、YUV422 等）当前渲染端不支持，明确拒绝而不是
    // 当成 8bit 送来导致画面错乱。
    out->m_format = DecodedPixelFormat::Unsupported;
    if (errorMessage != nullptr) {
        *errorMessage = "unsupported decoded pixel format " + std::to_string(frame->format);
    }
    return false;
}

}  // namespace

struct VideoDecoder::Impl {
    AVCodecContext *m_context = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_packet = nullptr;
    std::string m_lastError;
    int m_lastFfmpegError = 0;
    int m_decodeErrors = 0;
};

bool decodingAvailable() {
    return true;
}

VideoDecoder::VideoDecoder() : m_impl(new Impl()) {}

VideoDecoder::~VideoDecoder() {
    shutdown();
}

bool VideoDecoder::initialize(const std::string &codecName, std::string *errorMessage) {
    shutdown();

    // 只接受控制协议里出现的两个取值：其他名字说明调用方理解错了协议，
    // 静默当成 H.264 会让排查方向完全跑偏。
    if (codecName != "h264" && codecName != "h265") {
        if (errorMessage != nullptr) {
            *errorMessage = "unsupported codec name '" + codecName + "'";
        }
        return false;
    }
    const AVCodecID codecId = codecName == "h265" ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec *codec = avcodec_find_decoder(codecId);
    if (codec == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "ffmpeg has no decoder for codec " + codecName;
        }
        return false;
    }
    m_impl->m_context = avcodec_alloc_context3(codec);
    if (m_impl->m_context == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "avcodec_alloc_context3 failed";
        }
        return false;
    }
    // 低延迟优先：不开帧级多线程缓冲。默认多少，随后调试再调。
    m_impl->m_context->thread_count = 1;
    m_impl->m_context->flags |= AV_CODEC_FLAG_LOW_DELAY;

    const int openResult = avcodec_open2(m_impl->m_context, codec, nullptr);
    if (openResult < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "avcodec_open2 failed: " + ffmpegErrorText(openResult);
        }
        avcodec_free_context(&m_impl->m_context);
        return false;
    }

    m_impl->m_frame = av_frame_alloc();
    m_impl->m_packet = av_packet_alloc();
    if (m_impl->m_frame == nullptr || m_impl->m_packet == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "av_frame_alloc/av_packet_alloc failed";
        }
        shutdown();
        return false;
    }
    return true;
}

bool VideoDecoder::decodePacket(const std::uint8_t *data, std::size_t size,
                                std::vector<DecodedFrame> *frames,
                                std::string *errorMessage) {
    if (frames == nullptr || data == nullptr || size == 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "decodePacket requires a non-empty packet";
        }
        return false;
    }
    if (m_impl->m_context == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "decoder is not initialized";
        }
        return false;
    }
    frames->clear();

    const int sendResult = [&]() {
        av_packet_unref(m_impl->m_packet);
        if (av_new_packet(m_impl->m_packet, static_cast<int>(size)) < 0) {
            return AVERROR(ENOMEM);
        }
        std::memcpy(m_impl->m_packet->data, data, size);
        return avcodec_send_packet(m_impl->m_context, m_impl->m_packet);
    }();
    if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
        // 非关键帧在缺少参数集时解不出来属于正常现象（刚连上还没拿到 IDR），
        // 计数并继续，不当作致命错误。
        ++m_impl->m_decodeErrors;
        m_impl->m_lastFfmpegError = sendResult;
        m_impl->m_lastError = "send_packet: " + ffmpegErrorText(sendResult);
        if (errorMessage != nullptr) {
            *errorMessage = m_impl->m_lastError;
        }
        return false;
    }

    while (true) {
        const int receiveResult = avcodec_receive_frame(m_impl->m_context, m_impl->m_frame);
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
            break;  // 当前没有更多可出的帧
        }
        if (receiveResult < 0) {
            ++m_impl->m_decodeErrors;
            m_impl->m_lastFfmpegError = receiveResult;
            m_impl->m_lastError = "receive_frame: " + ffmpegErrorText(receiveResult);
            if (errorMessage != nullptr) {
                *errorMessage = m_impl->m_lastError;
            }
            return false;
        }
        DecodedFrame decoded;
        std::string copyError;
        if (!copyFrame(m_impl->m_frame, &decoded, &copyError)) {
            ++m_impl->m_decodeErrors;
            m_impl->m_lastError = copyError;
            if (errorMessage != nullptr) {
                *errorMessage = copyError;
            }
            av_frame_unref(m_impl->m_frame);
            return false;
        }
        frames->push_back(std::move(decoded));
        av_frame_unref(m_impl->m_frame);
    }
    return true;
}

void VideoDecoder::reset() {
    if (m_impl->m_context != nullptr) {
        // 清空参考帧与内部缓冲：断链后旧状态已经不可信。
        avcodec_flush_buffers(m_impl->m_context);
    }
}

void VideoDecoder::shutdown() {
    if (m_impl == nullptr) {
        return;
    }
    if (m_impl->m_packet != nullptr) {
        av_packet_free(&m_impl->m_packet);
    }
    if (m_impl->m_frame != nullptr) {
        av_frame_free(&m_impl->m_frame);
    }
    if (m_impl->m_context != nullptr) {
        avcodec_free_context(&m_impl->m_context);
    }
}

bool VideoDecoder::initialized() const {
    return m_impl->m_context != nullptr;
}

const std::string &VideoDecoder::lastError() const {
    return m_impl->m_lastError;
}

int VideoDecoder::lastFfmpegError() const {
    return m_impl->m_lastFfmpegError;
}

int VideoDecoder::decodeErrorCount() const {
    return m_impl->m_decodeErrors;
}

#else  // !SCLIENT_HAS_FFMPEG

struct VideoDecoder::Impl {
    std::string m_lastError;
    int m_lastFfmpegError = 0;
    int m_decodeErrors = 0;
};

bool decodingAvailable() {
    return false;
}

VideoDecoder::VideoDecoder() : m_impl(new Impl()) {}

VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::initialize(const std::string &, std::string *errorMessage) {
    m_impl->m_lastError = "built without ffmpeg: install libavcodec-dev and rebuild";
    if (errorMessage != nullptr) {
        *errorMessage = m_impl->m_lastError;
    }
    return false;
}

bool VideoDecoder::decodePacket(const std::uint8_t *, std::size_t,
                                std::vector<DecodedFrame> *, std::string *errorMessage) {
    if (errorMessage != nullptr) {
        *errorMessage = "built without ffmpeg";
    }
    return false;
}

void VideoDecoder::reset() {}
void VideoDecoder::shutdown() {}
bool VideoDecoder::initialized() const { return false; }
const std::string &VideoDecoder::lastError() const { return m_impl->m_lastError; }
int VideoDecoder::lastFfmpegError() const { return m_impl->m_lastFfmpegError; }
int VideoDecoder::decodeErrorCount() const { return m_impl->m_decodeErrors; }

#endif  // SCLIENT_HAS_FFMPEG

}  // namespace decoding
}  // namespace sclient
