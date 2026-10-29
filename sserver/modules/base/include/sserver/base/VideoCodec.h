// ============================================================
// 文件：VideoCodec.h
// 模块：sserver::base::codec
// 功能：统一 H.264 / H.265 的控制协议取值、NAL 类型提取、随机接入点
//       与参数集判定。编码档位、NAL 扫描、客户端诊断共用这一份语义。
// 知识点：强类型枚举、inline 头文件函数、输出参数、位掩码、
//         两种 NAL 头格式的差异
// ============================================================

#ifndef SSERVER_BASE_VIDEOCODEC_H
#define SSERVER_BASE_VIDEOCODEC_H

#include <cstddef> // size_t
#include <cstdint>
#include <string>

namespace sserver {
namespace base { 
namespace codec { 

// ==================== 编码种类与线格式取值 ====================

// 视频编码种类。控制协议 `codec=` 字段、媒体档位与 NAL 解析共用：
// x264 后端固定 kH264，MPP 后端由档位配置决定。
enum class VideoCodec {
    kH264,  /**< 参数集 SPS/PPS，随机接入为 IDR 切片 */
    kH265,  /**< 参数集 VPS/SPS/PPS，随机接入为 IRAP 类型范围 */
};

// 线格式字符串 "h264" / "h265"。与配置 codec.mpp_codec 取值一致，
// 也是控制协议 START 响应里回显的 codec 字段。
inline const char *videoCodecToWire(VideoCodec value) {
    return value == VideoCodec::kH265 ? "h265" : "h264";
}

// 仅在字符串完全匹配时写入结果；失败时保留调用方原值。
inline bool videoCodecFromWire(const std::string &value, VideoCodec *result) {
    if (result == nullptr) {
        return false;
    }
    if (value == "h264") {
        *result = VideoCodec::kH264;
        return true;
    }
    if (value == "h265") {
        *result = VideoCodec::kH265;
        return true;
    }
    return false;
}

// ==================== NAL 类型与语义判定 ====================

// 从 Annex B NAL 载荷的首字节起提取 NAL 单元类型。
// H.264 头占 1 字节，类型取低 5 位；H.265 头占 2 字节，类型取
// 首字节的 bit1..bit6（先右移再取低 6 位）。两者位宽不同，不能复用。
inline std::uint8_t nalUnitType(const std::uint8_t *data, std::size_t size, VideoCodec value) {
    if (data == nullptr || size == 0) {
        return 0;
    }
    if (value == VideoCodec::kH265) {
        if (size < 2) {
            return 0;
        }
        return static_cast<std::uint8_t>((data[0] >> 1) & 0x3FU);
    }
    return static_cast<std::uint8_t>(data[0] & 0x1FU);
}

// 随机接入点(关键帧（I帧 / IDR帧）)：H.264 只有 IDR（type 5）；H.265 为 IRAP 16..21
// （BLA / IDR / CRA）。H.264 的 type 1..4 是普通 VCL 切片，不是随机接入点(I帧)。
// 这里只判定类型，真正能否独立解码还取决于参数集与参考状态。
// 这些数字（5、7、8、16~21、32~34），在音视频开发里被称为 NAL Unit Type（NAL 单元类型）。
inline bool isRandomAccessNalType(std::uint8_t nalType, VideoCodec value) {
    if (value == VideoCodec::kH265) {
        return nalType >= 16 && nalType <= 21;
    }
    return nalType == 5;
}

// 参数集：H.264 为 SPS/PPS = 7/8；H.265 为 VPS/SPS/PPS = 32..34。
inline bool isParameterSetNalType(std::uint8_t nalType, VideoCodec value) {
    if (value == VideoCodec::kH265) {
        return nalType >= 32 && nalType <= 34;
    }
    return nalType == 7 || nalType == 8;
}

} // namespace codec
} // namespace base
} // namespace sserver

#endif