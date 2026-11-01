// FrameDiagnostics：起始码扫描、指纹一致性、NAL 收集与随机接入点检测的单元测试。
//
// 重点验证两条"跨端可比对"的性质：
//   1. 同一帧用 3 字节或 4 字节起始码封装，指纹必须相同；
//   2. 指纹必须区分「同前缀不同长度」的载荷（长度也参与哈希）。
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sserver/base/FrameDiagnostics.h"
#include "sserver/base/VideoCodec.h"
#include "support/Check.h"

namespace {

using sserver::base::analyzeFramePayload;
using sserver::base::findStartCode;
using sserver::base::formatFingerprint;
using sserver::base::kFrameDiagnosticSampleInterval;
using sserver::base::shouldSampleFrame;
using sserver::base::codec::VideoCodec;

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> result;
    result.reserve(values.size());
    for (int value : values) {
        result.push_back(static_cast<std::uint8_t>(value));
    }
    return result;
}

void testSamplingSchedule() {
    CHECK(shouldSampleFrame(0));   // 首帧总是采样，建立指纹基线
    CHECK(!shouldSampleFrame(1));
    CHECK(!shouldSampleFrame(kFrameDiagnosticSampleInterval - 1));
    CHECK(shouldSampleFrame(kFrameDiagnosticSampleInterval));
    CHECK(shouldSampleFrame(2 * kFrameDiagnosticSampleInterval));
}

void testFindStartCodeThreeByte() {
    const auto data = bytes({0x00, 0x00, 0x01, 0x65});
    std::size_t prefixSize = 0;
    CHECK(findStartCode(data.data(), data.size(), 0, &prefixSize) == 0);
    CHECK(prefixSize == 3);
}

void testFindStartCodeFourByte() {
    const auto data = bytes({0x00, 0x00, 0x00, 0x01, 0x65});
    std::size_t prefixSize = 0;
    CHECK(findStartCode(data.data(), data.size(), 0, &prefixSize) == 0);
    CHECK(prefixSize == 4);
}

void testFindStartCodeAbsent() {
    const auto data = bytes({0x11, 0x22, 0x33, 0x44});
    std::size_t prefixSize = 7;
    CHECK(findStartCode(data.data(), data.size(), 0, &prefixSize) == data.size());
    CHECK(prefixSize == 0);  // 未找到时清零，调用方不能沿用旧值

    // 空指针与空载荷都要安全返回 size，不能越界读。
    CHECK(findStartCode(nullptr, 0, 0, nullptr) == 0);
}

void testFindStartCodeRespectsOffset() {
    // 两个起始码，从第二个的位置开始找只能命中第二个。
    const auto data = bytes({0x00, 0x00, 0x01, 0x67, 0x00, 0x00, 0x01, 0x68});
    std::size_t prefixSize = 0;
    CHECK(findStartCode(data.data(), data.size(), 0, &prefixSize) == 0);
    CHECK(findStartCode(data.data(), data.size(), 4, &prefixSize) == 4);
    CHECK(prefixSize == 3);
}

void testFormatFingerprintIsFixedWidth() {
    // FNV-1a 的偏移基址，固定 16 位十六进制。
    CHECK(formatFingerprint(14695981039346656037ULL) == "fnv1a64:cbf29ce484222325");
    CHECK(formatFingerprint(0ULL) == "fnv1a64:0000000000000000");
}

void testRawPayloadWithoutStartCode() {
    const auto data = bytes({0x11, 0x22, 0x33});
    const auto result = analyzeFramePayload(data.data(), data.size());

    CHECK(result.m_framing == "raw");
    CHECK(result.m_nalTypes == "none");
    CHECK(result.m_nalCount == 0);
    CHECK(!result.m_hasIdr);
    CHECK(result.m_payloadFingerprint.rfind("fnv1a64:", 0) == 0);
}

void testAnnexBWithSpsPpsIdr() {
    const auto data = bytes({
            0x00, 0x00, 0x01, 0x67,   // SPS  类型 7
            0x00, 0x00, 0x01, 0x68,   // PPS  类型 8
            0x00, 0x00, 0x01, 0x65,   // IDR  类型 5
    });
    const auto result = analyzeFramePayload(data.data(), data.size());

    CHECK(result.m_framing == "annex_b");
    CHECK(result.m_nalCount == 3);
    CHECK(result.m_nalTypes == "7,8,5");
    CHECK(result.m_hasIdr);
}

void testNonKeySliceHasNoRandomAccessPoint() {
    // H.264 类型 1 是普通 VCL 切片，不是随机接入点。
    const auto data = bytes({0x00, 0x00, 0x01, 0x41});
    const auto result = analyzeFramePayload(data.data(), data.size());

    CHECK(result.m_nalCount == 1);
    CHECK(result.m_nalTypes == "1");
    CHECK(!result.m_hasIdr);
}

void testH265VpsAndIdr() {
    // H.265 两字节 NAL 头：VPS 类型 32 → 0x40；IDR_W_RADL 类型 19 → 0x26。
    const auto data = bytes({
            0x00, 0x00, 0x00, 0x01, 0x40, 0x01,
            0x00, 0x00, 0x00, 0x01, 0x26, 0x01,
    });
    const auto result = analyzeFramePayload(data.data(), data.size(), VideoCodec::kH265);

    CHECK(result.m_nalCount == 2);
    CHECK(result.m_nalTypes == "32,19");
    CHECK(result.m_hasIdr);
}

void testFingerprintIgnoresStartCodeLength() {
    // 同一 NAL 用 3 字节与 4 字节起始码封装，指纹必须一致——否则两端
    // 日志里同一帧的指纹对不上，这个诊断工具就没有意义了。
    const auto threeByte = bytes({0x00, 0x00, 0x01, 0x65});
    const auto fourByte = bytes({0x00, 0x00, 0x00, 0x01, 0x65});

    const auto first = analyzeFramePayload(threeByte.data(), threeByte.size());
    const auto second = analyzeFramePayload(fourByte.data(), fourByte.size());

    CHECK(first.m_payloadFingerprint == second.m_payloadFingerprint);
    CHECK(first.m_nalTypes == second.m_nalTypes);
    CHECK(first.m_nalCount == second.m_nalCount);
}

void testFingerprintDistinguishesLength() {
    // 同内容不同长度：长度参与哈希，指纹必须不同。
    const auto shortPayload = bytes({0x00, 0x00, 0x01, 0x65, 0x11});
    const auto longPayload = bytes({0x00, 0x00, 0x01, 0x65, 0x11, 0x22});

    const auto first = analyzeFramePayload(shortPayload.data(), shortPayload.size());
    const auto second = analyzeFramePayload(longPayload.data(), longPayload.size());
    CHECK(first.m_payloadFingerprint != second.m_payloadFingerprint);

    // 同一份载荷算两次必须稳定。
    const auto again = analyzeFramePayload(shortPayload.data(), shortPayload.size());
    CHECK(first.m_payloadFingerprint == again.m_payloadFingerprint);
}

void testNalTypesTruncatedAfterSixteen() {
    std::vector<std::uint8_t> data;
    constexpr int kNalCount = 20;
    for (int index = 0; index < kNalCount; ++index) {
        data.push_back(0x00);
        data.push_back(0x00);
        data.push_back(0x01);
        data.push_back(0x41);  // 类型 1
    }

    const auto result = analyzeFramePayload(data.data(), data.size());
    CHECK(result.m_nalCount == static_cast<std::size_t>(kNalCount));
    // 前 16 个类型逐条列出，之后用 ",..." 表示截断。
    CHECK(result.m_nalTypes ==
          "1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,...");
}

void testEmptyPayload() {
    const auto result = analyzeFramePayload(nullptr, 0);
    CHECK(result.m_framing == "raw");
    CHECK(result.m_nalCount == 0);
    CHECK(result.m_nalTypes == "none");
    CHECK(!result.m_hasIdr);
}

}  // namespace

int main() {
    testSamplingSchedule();
    testFindStartCodeThreeByte();
    testFindStartCodeFourByte();
    testFindStartCodeAbsent();
    testFindStartCodeRespectsOffset();
    testFormatFingerprintIsFixedWidth();
    testRawPayloadWithoutStartCode();
    testAnnexBWithSpsPpsIdr();
    testNonKeySliceHasNoRandomAccessPoint();
    testH265VpsAndIdr();
    testFingerprintIgnoresStartCodeLength();
    testFingerprintDistinguishesLength();
    testNalTypesTruncatedAfterSixteen();
    testEmptyPayload();
    return sserver::test::finish("frame_diagnostics_test");
}
