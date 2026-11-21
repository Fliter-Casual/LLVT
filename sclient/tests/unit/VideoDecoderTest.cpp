// VideoDecoder：解码器初始化与异常输入的单元测试。
//
// 构建是否链接 FFmpeg 决定跑哪条分支：
//   - 未链接：initialize 必须明确失败并给出可操作的原因
//   - 已链接：h264 初始化成功、非法 codec 名被拒、垃圾输入不崩、
//             reset 后可继续使用
// 真正的"能解出画面"需要真实码流，由双端联调覆盖——单测这里守的是
// 边界行为，不是编码质量。
#include <cstdint>
#include <string>
#include <vector>

#include "sclient/decoding/VideoDecoder.h"
#include "support/Check.h"

namespace {

using sclient::decoding::DecodedFrame;
using sclient::decoding::decodingAvailable;
using sclient::decoding::VideoDecoder;

void testUnavailableBuildFailsLoudly() {
    if (decodingAvailable()) {
        return;
    }
    VideoDecoder decoder;
    std::string error;
    CHECK(!decoder.initialize("h264", &error));
    CHECK(error.find("ffmpeg") != std::string::npos);
    CHECK(!decoder.initialized());
}

void testInitializeAndShutdown() {
    if (!decodingAvailable()) {
        return;
    }
    VideoDecoder decoder;
    std::string error;
    CHECK(decoder.initialize("h264", &error));
    CHECK(decoder.initialized());

    // 重复初始化：先关旧上下文再开新的，不能泄漏也不能失败。
    CHECK(decoder.initialize("h264", &error));
    CHECK(decoder.initialized());

    decoder.shutdown();
    CHECK(!decoder.initialized());
}

void testRejectsUnknownCodecName() {
    if (!decodingAvailable()) {
        return;
    }
    VideoDecoder decoder;
    std::string error;
    // 控制协议只会有 h264/h265；其他名字说明调用方理解错了，必须拒绝，
    // 静默当成 H.264 会让排查方向跑偏。
    CHECK(!decoder.initialize("h266", &error));
    CHECK(error.find("unsupported codec") != std::string::npos);
    CHECK(!decoder.initialized());
}

void testGarbageInputDoesNotCrash() {
    if (!decodingAvailable()) {
        return;
    }
    VideoDecoder decoder;
    std::string error;
    CHECK(decoder.initialize("h264", &error));

    // 任意字节都不应该让解码器崩溃：可能报错，也可能静默吞掉（参数集缺失），
    // 但绝不能越界读。
    std::vector<std::uint8_t> garbage(256, 0xAB);
    std::vector<DecodedFrame> frames;
    const bool accepted = decoder.decodePacket(garbage.data(), garbage.size(), &frames, &error);
    if (!accepted) {
        CHECK(!error.empty());
        CHECK(decoder.decodeErrorCount() >= 1);
    } else {
        CHECK(frames.empty());
    }

    // 断链重置后必须仍可继续喂数据（不是一次性对象）。
    decoder.reset();
    frames.clear();
    decoder.decodePacket(garbage.data(), garbage.size(), &frames, &error);
    decoder.shutdown();
}

void testRejectsEmptyPacket() {
    if (!decodingAvailable()) {
        return;
    }
    VideoDecoder decoder;
    std::string error;
    CHECK(decoder.initialize("h264", &error));
    std::vector<DecodedFrame> frames;
    CHECK(!decoder.decodePacket(nullptr, 0, &frames, &error));
    CHECK(!error.empty());
}

void testDecodeBeforeInitializeFails() {
    VideoDecoder decoder;
    std::string error;
    std::vector<std::uint8_t> data(16, 0);
    std::vector<DecodedFrame> frames;
    // 失败原因随构建而不同（未链接 FFmpeg / 未初始化），但必须给出原因，
    // 不能静默返回"没有帧"。
    CHECK(!decoder.decodePacket(data.data(), data.size(), &frames, &error));
    CHECK(!error.empty());
}

}  // namespace

int main() {
    testUnavailableBuildFailsLoudly();
    testInitializeAndShutdown();
    testRejectsUnknownCodecName();
    testGarbageInputDoesNotCrash();
    testRejectsEmptyPacket();
    testDecodeBeforeInitializeFails();
    return sclient::test::finish("video_decoder_test");
}
