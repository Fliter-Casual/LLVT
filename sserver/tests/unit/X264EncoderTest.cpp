// X264Encoder：软编后端的单元测试。
//
// 构建是否链接 libx264 决定跑哪条分支：
//   - 未链接：initialize 必须明确失败并给出可操作的原因（不能静默退化）
//   - 已链接：真编两帧，验证 Annex B 起始码、首帧 IDR、强制关键帧与关闭后的拒绝
// 同一份用例在两种构建下都成立，所以开发机和目标机都能跑。
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sserver/appconfig/CodecConfig.h"
#include "sserver/base/RawVideoFrame.h"
#include "sserver/encoding/X264Encoder.h"
#include "support/Check.h"

namespace {

using sserver::encoding::X264Encoder;

sserver::appconfig::CodecConfig makeCodecConfig() {
    sserver::appconfig::CodecConfig config;
    config.m_bitrateBps = 800000;
    config.m_gopFrames = 25;
    config.m_encodeThreads = 1;
    config.m_scenecut = false;
    return config;
}

// 构造一帧可预测的 YUYV：左半亮、右半暗，色度恒定。
std::vector<std::uint8_t> makeYuyv(int width, int height) {
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(width) * height * 2U, 0);
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; column += 2) {
            const std::size_t offset =
                    (static_cast<std::size_t>(row) * width + static_cast<std::size_t>(column)) * 2U;
            const std::uint8_t luma = column < width / 2 ? 200 : 40;
            frame[offset] = luma;
            frame[offset + 1] = 128;
            frame[offset + 2] = luma;
            frame[offset + 3] = 128;
        }
    }
    return frame;
}

bool startsWithStartCode(const std::vector<std::uint8_t> &data) {
    return data.size() >= 4 && data[0] == 0x00 && data[1] == 0x00
            && data[2] == 0x00 && data[3] == 0x01;
}

void testUnavailableBuildFailsLoudly() {
    X264Encoder encoder;
    std::string error;
    // 未链接 libx264 时两条断言才生效；链接了则这个用例跳过。
    if (sserver::encoding::x264Available()) {
        return;
    }
    CHECK(!encoder.initialize(64, 64, 25, makeCodecConfig(), &error));
    CHECK(error.find("libx264") != std::string::npos);
}

void testEncodeSequence() {
    if (!sserver::encoding::x264Available()) {
        return;
    }

    constexpr int kWidth = 64;
    constexpr int kHeight = 64;
    X264Encoder encoder;
    std::string error;
    CHECK(encoder.initialize(kWidth, kHeight, 25, makeCodecConfig(), &error));

    // 首帧必须是 IDR：否则客户端拿到的第一批数据建不起参考链。
    sserver::base::RawVideoFrameView view;
    view.m_pixelFormat = sserver::base::RawVideoPixelFormat::Yuyv422;
    const std::vector<std::uint8_t> pixels = makeYuyv(kWidth, kHeight);
    view.m_data = pixels.data();
    view.m_bytesUsed = pixels.size();
    view.m_width = kWidth;
    view.m_height = kHeight;
    view.m_stride = static_cast<std::size_t>(kWidth) * 2U;

    std::vector<std::uint8_t> output;
    bool isKeyframe = false;
    CHECK(encoder.encodeFrame(view, &output, &isKeyframe, &error));
    CHECK(isKeyframe);
    CHECK(startsWithStartCode(output));
    CHECK(output.size() > 16);

    // 第二帧在 GOP 内，应当是非关键帧。
    CHECK(encoder.encodeFrame(view, &output, &isKeyframe, &error));
    CHECK(!isKeyframe);
    CHECK(startsWithStartCode(output));

    // 强制关键帧：下一帧必须是 IDR（客户端丢包恢复靠它）。
    encoder.forceKeyframe();
    CHECK(encoder.encodeFrame(view, &output, &isKeyframe, &error));
    CHECK(isKeyframe);

    // 尺寸不符的输入必须拒绝，而不是读越界。
    view.m_bytesUsed = 16;
    CHECK(!encoder.encodeFrame(view, &output, &isKeyframe, &error));

    encoder.shutdown();
    // 关闭后再编码必须失败：调用方按错误处理，不能拿到半截码流。
    CHECK(!encoder.encodeYuyv422Frame(pixels.data(), pixels.size(), &output, &isKeyframe, &error));
}

void testRejectsOddDimensions() {
    if (!sserver::encoding::x264Available()) {
        return;
    }
    X264Encoder encoder;
    std::string error;
    // 4:2:0 要求宽高为偶，奇数尺寸在入口就拒绝。
    CHECK(!encoder.initialize(63, 64, 25, makeCodecConfig(), &error));
    CHECK(error.find("even") != std::string::npos);
}

}  // namespace

int main() {
    testUnavailableBuildFailsLoudly();
    testEncodeSequence();
    testRejectsOddDimensions();
    return sserver::test::finish("x264_encoder_test");
}
