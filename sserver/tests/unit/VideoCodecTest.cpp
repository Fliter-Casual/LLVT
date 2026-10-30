// VideoCodec：线格式取值与 NAL 类型判定的单元测试。
//
// 覆盖两种编码的 NAL 头宽度差异（H.264 低 5 位 / H.265 右移一位取低 6 位）、
// 随机接入点范围、参数集范围，以及非法线格式字符串不改变调用方原值。
#include <cstdint>
#include <string>

#include "sserver/base/VideoCodec.h"
#include "support/Check.h"

namespace {

using sserver::base::codec::VideoCodec;

void testWireConversion() {
    CHECK(std::string(sserver::base::codec::videoCodecToWire(VideoCodec::kH264)) == "h264");
    CHECK(std::string(sserver::base::codec::videoCodecToWire(VideoCodec::kH265)) == "h265");

    VideoCodec parsed = VideoCodec::kH264;
    CHECK(sserver::base::codec::videoCodecFromWire("h265", &parsed));
    CHECK(parsed == VideoCodec::kH265);
    CHECK(sserver::base::codec::videoCodecFromWire("h264", &parsed));
    CHECK(parsed == VideoCodec::kH264);

    // 失败时保留原值，空指针安全返回 false。
    CHECK(!sserver::base::codec::videoCodecFromWire("h266", &parsed));
    CHECK(parsed == VideoCodec::kH264);
    CHECK(!sserver::base::codec::videoCodecFromWire("h264", nullptr));
}

void testNalUnitTypeH264() {
    // 0x65 = forbidden_zero_bit 0 | nal_ref_idc 3 | type 5（IDR）
    const std::uint8_t idr = 0x65;
    CHECK(sserver::base::codec::nalUnitType(&idr, 1, VideoCodec::kH264) == 5);
    // 0x67 = SPS，类型 7
    const std::uint8_t sps = 0x67;
    CHECK(sserver::base::codec::nalUnitType(&sps, 1, VideoCodec::kH264) == 7);

    CHECK(sserver::base::codec::nalUnitType(nullptr, 4, VideoCodec::kH264) == 0);
    CHECK(sserver::base::codec::nalUnitType(&idr, 0, VideoCodec::kH264) == 0);
}

void testNalUnitTypeH265() {
    // H.265 头两字节：type << 1 位于首字节高位。VPS 类型 32 → 0x40。
    const std::uint8_t vps[2] = {0x40, 0x01};
    CHECK(sserver::base::codec::nalUnitType(vps, 2, VideoCodec::kH265) == 32);
    // IDR_W_RADL 类型 19 → 19 << 1 = 0x26。
    const std::uint8_t idr[2] = {0x26, 0x01};
    CHECK(sserver::base::codec::nalUnitType(idr, 2, VideoCodec::kH265) == 19);

    // 不足两字节无法取类型；两字节头是 H.265 与 H.264 的关键差异。
    CHECK(sserver::base::codec::nalUnitType(vps, 1, VideoCodec::kH265) == 0);
    CHECK(sserver::base::codec::nalUnitType(nullptr, 2, VideoCodec::kH265) == 0);
}

void testRandomAccessTypes() {
    CHECK(sserver::base::codec::isRandomAccessNalType(5, VideoCodec::kH264));
    CHECK(!sserver::base::codec::isRandomAccessNalType(1, VideoCodec::kH264));
    CHECK(!sserver::base::codec::isRandomAccessNalType(7, VideoCodec::kH264));

    // H.265 IRAP 覆盖 BLA(16..18) / IDR(19..20) / CRA(21)。
    for (std::uint8_t type = 16; type <= 21; ++type) {
        CHECK(sserver::base::codec::isRandomAccessNalType(type, VideoCodec::kH265));
    }
    CHECK(!sserver::base::codec::isRandomAccessNalType(15, VideoCodec::kH265));
    CHECK(!sserver::base::codec::isRandomAccessNalType(22, VideoCodec::kH265));
}

void testParameterSetTypes() {
    CHECK(sserver::base::codec::isParameterSetNalType(7, VideoCodec::kH264));
    CHECK(sserver::base::codec::isParameterSetNalType(8, VideoCodec::kH264));
    CHECK(!sserver::base::codec::isParameterSetNalType(5, VideoCodec::kH264));

    // H.265 比 H.264 多一个 VPS。
    CHECK(sserver::base::codec::isParameterSetNalType(32, VideoCodec::kH265));
    CHECK(sserver::base::codec::isParameterSetNalType(33, VideoCodec::kH265));
    CHECK(sserver::base::codec::isParameterSetNalType(34, VideoCodec::kH265));
    CHECK(!sserver::base::codec::isParameterSetNalType(35, VideoCodec::kH265));
}

}  // namespace

int main() {
    testWireConversion();
    testNalUnitTypeH264();
    testNalUnitTypeH265();
    testRandomAccessTypes();
    testParameterSetTypes();
    return sserver::test::finish("video_codec_test");
}
