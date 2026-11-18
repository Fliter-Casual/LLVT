// ============================================================
// 文件：X264Encoder.cpp
// 模块：sserver::encoding
// 功能：libx264 后端的实现：初始化参数、逐帧把 YUYV 转 I420 后编码，
//       输出带起始码的 Annex B 码流。未链接 libx264 时提供失败实现。
// 知识点：x264_param 调优（zerolatency / 固定 GOP / 小 VBV）、
//         x264_picture_alloc、NAL 拼装、强制 IDR
// ============================================================

#include "sserver/encoding/X264Encoder.h"

#include <atomic>
#include <cstring>
#include <utility>

#include "sserver/encoding/I420Convert.h"

#if defined(SSERVER_HAS_X264)
#include <x264.h>
#endif

namespace sserver {
namespace encoding {

#if defined(SSERVER_HAS_X264)

// ==================== 有 libx264 的实现 ====================

struct X264Encoder::Impl {
    x264_t *m_encoder = nullptr;
    x264_param_t m_param{};
    x264_picture_t m_picture{};
    std::vector<std::uint8_t> m_i420Scratch[3];
    int m_width = 0;
    int m_height = 0;
    int m_gopFrames = 30;
    std::atomic<bool> m_forceKeyframe{true};
    std::atomic<std::int64_t> m_pendingBitrate{0};
    std::uint64_t m_frameIndex = 0;
};

bool x264Available() {
    return true;
}

X264Encoder::X264Encoder() : m_impl(new Impl()) {}

X264Encoder::~X264Encoder() {
    shutdown();
}

bool X264Encoder::initialize(int width, int height, int fps,
                             const appconfig::CodecConfig &config,
                             std::string *errorMessage) {
    shutdown();
    if (width <= 0 || height <= 0 || width % 2 != 0 || height % 2 != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "x264 requires positive even dimensions";
        }
        return false;
    }

    x264_param_t &param = m_impl->m_param;
    // zerolatency 关掉前瞻与 B 帧：延迟优先于压缩率。veryfast 是软编在
    // 1080p 上还能跑满帧率的档位。
    if (x264_param_default_preset(&param, "veryfast", "zerolatency") < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "x264_param_default_preset failed";
        }
        return false;
    }

    param.i_csp = X264_CSP_I420;
    param.i_width = width;
    param.i_height = height;
    param.i_fps_num = fps > 0 ? fps : 25;
    param.i_fps_den = 1;
    param.b_vfr_input = 0;
    param.b_annexb = 1;   // 输出自带 00 00 00 01 起始码

    // 固定 GOP：只把 keyint 设成同一个值是不够的，x264 会把最小间隔钳到
    // 一半加一，而且场景切换检测默认开着、画面一变就中途插关键帧。
    param.i_keyint_max = config.m_gopFrames > 0 ? config.m_gopFrames : 30;
    param.i_keyint_min = param.i_keyint_max;
    param.i_scenecut_threshold = config.m_scenecut ? 40 : 0;
    m_impl->m_gopFrames = param.i_keyint_max;

    // VBV 故意配小：缓冲模型越大，解码端就要预留越大的抖动空间，延迟越高。
    const int bitrateKbps = static_cast<int>((config.m_bitrateBps > 0 ? config.m_bitrateBps : 800000) / 1000);
    param.rc.i_rc_method = X264_RC_ABR;
    param.rc.i_bitrate = bitrateKbps;
    param.rc.i_vbv_max_bitrate = bitrateKbps;
    param.rc.i_vbv_buffer_size = 300;   // 300 kbit ≈ 0.3 秒额度

    param.i_threads = config.m_encodeThreads > 0 ? config.m_encodeThreads : 1;
    param.b_sliced_threads = 1;

    if (x264_param_apply_profile(&param, "baseline") < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "x264_param_apply_profile(baseline) failed";
        }
        return false;
    }

    m_impl->m_encoder = x264_encoder_open(&param);
    if (m_impl->m_encoder == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "x264_encoder_open failed";
        }
        return false;
    }

    if (x264_picture_alloc(&m_impl->m_picture, X264_CSP_I420, width, height) < 0) {
        x264_encoder_close(m_impl->m_encoder);
        m_impl->m_encoder = nullptr;
        if (errorMessage != nullptr) {
            *errorMessage = "x264_picture_alloc failed";
        }
        return false;
    }

    m_impl->m_width = width;
    m_impl->m_height = height;
    m_impl->m_frameIndex = 0;
    m_impl->m_forceKeyframe.store(true);  // 首帧必须是 IDR，否则解码器建不了链
    return true;
}

void X264Encoder::forceKeyframe() {
    m_impl->m_forceKeyframe.store(true);
}

void X264Encoder::setBitrate(std::int64_t bitrateBps) {
    // 只在帧边界应用：x264 的参数结构在编码中修改会与编码调用竞态。
    m_impl->m_pendingBitrate.store(bitrateBps);
}

bool X264Encoder::encodeYuyv422Frame(const std::uint8_t *input,
                                     std::size_t inputLength,
                                     std::vector<std::uint8_t> *output,
                                     bool *isKeyframe,
                                     std::string *errorMessage) {
    if (output == nullptr || isKeyframe == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "encode requires output buffers";
        }
        return false;
    }
    if (m_impl->m_encoder == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "x264 encoder is not initialized";
        }
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(m_impl->m_width)
            * static_cast<std::size_t>(m_impl->m_height) * 2U;
    if (input == nullptr || inputLength < expected) {
        if (errorMessage != nullptr) {
            *errorMessage = "input frame is smaller than the configured size";
        }
        return false;
    }

    const std::int64_t pendingBitrate = m_impl->m_pendingBitrate.exchange(0);
    if (pendingBitrate > 0) {
        m_impl->m_param.rc.i_bitrate = static_cast<int>(pendingBitrate / 1000);
        m_impl->m_param.rc.i_vbv_max_bitrate = m_impl->m_param.rc.i_bitrate;
        x264_encoder_reconfig(m_impl->m_encoder, &m_impl->m_param);
    }

    // YUYV → I420，目标直接写进 x264 的 picture 平面（行距取 picture 的）。
    I420Planes planes;
    planes.m_y = m_impl->m_picture.img.plane[0];
    planes.m_u = m_impl->m_picture.img.plane[1];
    planes.m_v = m_impl->m_picture.img.plane[2];
    planes.m_yStride = static_cast<std::size_t>(m_impl->m_picture.img.i_stride[0]);
    planes.m_uStride = static_cast<std::size_t>(m_impl->m_picture.img.i_stride[1]);
    planes.m_vStride = static_cast<std::size_t>(m_impl->m_picture.img.i_stride[2]);
    if (!convertYuyv422ToI420(input, static_cast<std::size_t>(m_impl->m_width) * 2U, planes,
                              m_impl->m_width, m_impl->m_height)) {
        if (errorMessage != nullptr) {
            *errorMessage = "YUYV to I420 conversion failed";
        }
        return false;
    }

    if (m_impl->m_forceKeyframe.exchange(false)) {
        m_impl->m_picture.i_type = X264_TYPE_IDR;
    } else {
        m_impl->m_picture.i_type = X264_TYPE_AUTO;
    }
    m_impl->m_picture.i_pts = static_cast<std::int64_t>(m_impl->m_frameIndex++);

    x264_nal_t *nals = nullptr;
    int nalCount = 0;
    x264_picture_t pictureOut{};
    const int frameBytes = x264_encoder_encode(m_impl->m_encoder, &nals, &nalCount,
                                               &m_impl->m_picture, &pictureOut);
    if (frameBytes < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "x264_encoder_encode failed";
        }
        return false;
    }
    if (frameBytes == 0) {
        // zerolatency + sliced_threads 下基本不会走到这里；真出现说明编码器
        // 内部还在缓冲，调用方按"本帧没有产出"处理即可，不算失败。
        output->clear();
        *isKeyframe = false;
        return true;
    }

    output->clear();
    output->reserve(static_cast<std::size_t>(frameBytes));
    for (int index = 0; index < nalCount; ++index) {
        const x264_nal_t &nal = nals[index];
        output->insert(output->end(), nal.p_payload, nal.p_payload + nal.i_payload);
    }
    *isKeyframe = pictureOut.b_keyframe != 0;
    return true;
}

void X264Encoder::shutdown() {
    if (m_impl == nullptr) {
        return;
    }
    if (m_impl->m_encoder != nullptr) {
        x264_picture_clean(&m_impl->m_picture);
        x264_encoder_close(m_impl->m_encoder);
        m_impl->m_encoder = nullptr;
    }
}

#else  // !SSERVER_HAS_X264

// ==================== 未链接 libx264 时的失败实现 ====================

struct X264Encoder::Impl {};

bool x264Available() {
    return false;
}

X264Encoder::X264Encoder() : m_impl(new Impl()) {}

X264Encoder::~X264Encoder() = default;

bool X264Encoder::initialize(int, int, int, const appconfig::CodecConfig &,
                             std::string *errorMessage) {
    // 明确失败而不是静默退回合成编码器：合成码流不是合法 H.264，
    // 客户端解不出来，那种"能跑但看不出为什么"的状态最浪费时间。
    if (errorMessage != nullptr) {
        *errorMessage = "built without libx264: install libx264-dev and rebuild";
    }
    return false;
}

void X264Encoder::forceKeyframe() {}

void X264Encoder::setBitrate(std::int64_t) {}

bool X264Encoder::encodeYuyv422Frame(const std::uint8_t *, std::size_t,
                                     std::vector<std::uint8_t> *,
                                     bool *isKeyframe,
                                     std::string *errorMessage) {
    if (errorMessage != nullptr) {
        *errorMessage = "built without libx264";
    }
    if (isKeyframe != nullptr) {
        *isKeyframe = false;
    }
    return false;
}

void X264Encoder::shutdown() {}

#endif  // SSERVER_HAS_X264

}  // namespace encoding
}  // namespace sserver
