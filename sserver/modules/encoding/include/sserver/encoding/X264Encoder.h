// ============================================================
// 文件：X264Encoder.h
// 模块：sserver::encoding
// 功能：基于 libx264 的软编后端：YUYV422 → I420 → H.264 Annex B。
//       未链接 libx264 构建时，initialize 直接失败并给出明确原因。
// 知识点：pimpl 隐藏第三方头文件、zerolatency 调参、固定 GOP、
//         强制 IDR、Annex B NAL 拼装
// ============================================================

#ifndef SSERVER_ENCODING_X264_ENCODER_H
#define SSERVER_ENCODING_X264_ENCODER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sserver/encoding/Encoder.h"

namespace sserver {
namespace encoding {

// 构建时是否链接了 libx264。上层据此决定用真编码器还是合成替身，
// 而不是等到 initialize 失败才发现。
bool x264Available();

// 公开头文件里不出现 x264.h：第三方类型全部藏在 Impl 里，换库版本不会
// 波及包含本头的所有模块。
class X264Encoder final : public Encoder {
public:
    X264Encoder();
    ~X264Encoder() override;

    X264Encoder(const X264Encoder &) = delete;
    X264Encoder &operator=(const X264Encoder &) = delete;

    bool initialize(int width, int height, int fps,
                    const appconfig::CodecConfig &config,
                    std::string *errorMessage) override;
    void forceKeyframe() override;
    void setBitrate(std::int64_t bitrateBps) override;
    bool encodeYuyv422Frame(const std::uint8_t *input,
                            std::size_t inputLength,
                            std::vector<std::uint8_t> *output,
                            bool *isKeyframe,
                            std::string *errorMessage) override;
    void shutdown() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace encoding
}  // namespace sserver

#endif  // SSERVER_ENCODING_X264_ENCODER_H
