// 帧诊断
// ============================================================
// 文件：FrameDiagnostics.h
// 模块：sserver::base
// 功能：视频帧诊断工具：Annex B 起始码扫描、FNV-1a 64 位指纹、NAL 类型
//       收集与随机接入点检测，输出「帧指纹 + 封装格式 + NAL 列表」摘要，
//       供编码与传输两侧日志做跨端关联比对。
// 知识点：H.264/H.265 Annex B 起始码、FNV-1a 增量哈希、NAL 单元类型、
//         header-only 工具函数、constexpr 常量
// ============================================================

#ifndef SSERVER_BASE_FRAME_DIAGNOSTICS_H
#define SSERVER_BASE_FRAME_DIAGNOSTICS_H

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include "sserver/base/VideoCodec.h"

namespace sserver {
namespace base {

// ==================== 采样控制与诊断结果 ====================

// 诊断不必逐帧做：哈希整个载荷、拼 NAL 列表都是每帧几十微秒的开销，
// 而问题排查只需要低频抽样。120 帧(视频由120张图片组成)在 30fps(帧率:1s显示30张图片) 下约每 4 秒一个样本。
constexpr std::uint64_t kFrameDiagnosticSampleInterval = 120; //constexpr常量表达式：在编译期计算出结果

// 一次载荷分析的结果：指纹与 NAL 列表都是日志用字符串，不含帧数据本体。
struct FrameDiagnostics {
    std::string m_payloadFingerprint; /**< "fnv1a64:" + 16 位十六进制 */
    std::string m_framing;            /**< "annex_b" 或 "raw" */
    std::string m_nalTypes;           /**< 逗号分隔的 NAL 类型，超过 16 个截断 */
    std::size_t m_nalCount = 0;
    bool m_hasIdr = false;            /**< 含 H.264 IDR 或 H.265 IRAP */
};

// 首帧总是采样：开局就建立指纹基线，便于和后续样本比对。
inline bool shouldSampleFrame(std::uint64_t frameId) {
    return frameId == 0 || frameId % kFrameDiagnosticSampleInterval == 0;
}

// ==================== Annex B 起始码扫描 ====================

// 在 data[offset, size) 内查找 Annex B 起始码（3 字节 0x000001 或
// 4 字节 0x00000001）。先判 3 字节：4 字节起始码的前导零在下一轮扫描时
// 会自然被跳过，两种都能识别。
//
// @param prefixSize 非空时输出起始码长度（3 或 4）
// @return 起始码首字节下标；未找到返回 size
inline std::size_t findStartCode(const std::uint8_t *data,
                                 std::size_t size,
                                 std::size_t offset,
                                 std::size_t *prefixSize) {
    if (prefixSize != nullptr) {
        *prefixSize = 0;
    }
    if (data == nullptr) {
        return size;
    }
    // 循环条件保证 index+2 不越界；4 字节分支再单独校验 index+3。
    for (std::size_t index = offset; index + 3 <= size; ++index) {
        if (data[index] != 0 || data[index + 1] != 0) {
            continue;
        }
        if (data[index + 2] == 1) {
            if (prefixSize != nullptr) {
                *prefixSize = 3;
            }
            return index;
        }
        if (index + 4 <= size && data[index + 2] == 0 && data[index + 3] == 1) {
            if (prefixSize != nullptr) {
                *prefixSize = 4;
            }
            return index;
        }
    }
    return size;
}

// ==================== FNV-1a 指纹原语 ====================

// FNV-1a 64 位：逐字节「异或再乘素数」。做成增量式（外部持有 hash 状态）
// 是为了能分多段喂入——先喂一个类型标记、再喂长度、再喂净荷——结果与
// 一次性喂入完整字节流一致。
//
// 非密码学哈希：只用于日志里逐字符比对两端的同一帧，不承担完整性校验。
constexpr std::uint64_t kFnv1a64OffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;

inline void hashByte(std::uint8_t value, std::uint64_t *hash) {
    *hash ^= value;
    *hash *= kFnv1a64Prime;
}

// 长度也按 8 字节小端逐字节喂入：不同长度但前缀相同的载荷必须得到
// 不同指纹，否则截断的帧会和完整的帧撞在一起。
inline void hashSize(std::size_t value, std::uint64_t *hash) {
    const std::uint64_t kFixed = static_cast<std::uint64_t>(value);
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        hashByte(static_cast<std::uint8_t>((kFixed >> shift) & 0xFFU), hash);
    }
}

inline void hashBytes(const std::uint8_t *data, std::size_t size, std::uint64_t *hash) {
    for (std::size_t index = 0; index < size; ++index) {
        hashByte(data[index], hash);
    }
}

// ==================== 指纹格式化与 NAL 类型收集 ====================

// 固定 "fnv1a64:" + 定宽 16 位十六进制：日志里的指纹必须逐字符可比对，
// 短写会与补零写法不一致。
inline std::string formatFingerprint(std::uint64_t hash) {
    std::ostringstream stream;
    stream << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

// 只记录前 16 个 NAL 类型，超出部分用 ",..." 表示截断：
// 一帧可能有几十个切片 NAL，全打出来日志就没法看了。
inline void appendNalType(std::uint8_t nalType, std::size_t nalIndex, std::string *types) {
    constexpr std::size_t kMaxLoggedNalTypes = 16;
    if (nalIndex >= kMaxLoggedNalTypes) {
        if (nalIndex == kMaxLoggedNalTypes) {
            types->append(",...");
        }
        return;
    }
    if (!types->empty()) {
        types->push_back(',');
    }
    types->append(std::to_string(nalType));
}

// ==================== 帧载荷分析 ====================

// 指纹按「NAL 边界 + 净荷」计算，跳过 3/4 字节起始码和尾部 zero_byte：
// 同一帧用不同起始码长度封装时指纹必须一致，否则两端日志对不上。
//
// 三种喂入标记区分段类型（0x52 'R' 表示原始字节、0x4E 'N' 表示 NAL），
// 避免「raw 载荷」与「单个 NAL 载荷」在哈希空间里撞车。
//
// 本函数每次调用都完整分析载荷，是否抽样由调用方用 shouldSampleFrame 决定。
inline FrameDiagnostics analyzeFramePayload(const std::uint8_t *data,
                                            std::size_t size,
                                            codec::VideoCodec codec = codec::VideoCodec::kH264) {
    FrameDiagnostics result;
    std::uint64_t hash = kFnv1a64OffsetBasis;

    std::size_t prefixSize = 0;
    std::size_t start = findStartCode(data, size, 0, &prefixSize);
    if (start == size) {
        // 没有起始码：按原始字节整体处理（例如直接把压缩帧当载荷的调试路径）。
        result.m_framing = "raw";
        hashByte(0x52, &hash);
        hashSize(size, &hash);
        if (data != nullptr) {
            hashBytes(data, size, &hash);
        }
        result.m_payloadFingerprint = formatFingerprint(hash);
        result.m_nalTypes = "none";
        return result;
    }

    result.m_framing = "annex_b";
    while (start < size) {
        const std::size_t kPayloadStart = start + prefixSize;
        std::size_t nextPrefixSize = 0;
        const std::size_t kNext = findStartCode(data, size, kPayloadStart, &nextPrefixSize);

        // 尾部零字节是下一个起始码的 zero_byte 前缀，不属于本 NAL 净荷。
        std::size_t payloadEnd = kNext;
        while (payloadEnd > kPayloadStart && data[payloadEnd - 1] == 0) {
            --payloadEnd;
        }

        if (payloadEnd > kPayloadStart) {
            const std::size_t kPayloadSize = payloadEnd - kPayloadStart;
            const std::uint8_t kNalType =
                    codec::nalUnitType(data + kPayloadStart, kPayloadSize, codec);
            hashByte(0x4E, &hash);
            hashSize(kPayloadSize, &hash);
            hashBytes(data + kPayloadStart, kPayloadSize, &hash);

            appendNalType(kNalType, result.m_nalCount, &result.m_nalTypes);
            ++result.m_nalCount;
            if (codec::isRandomAccessNalType(kNalType, codec)) {
                result.m_hasIdr = true;
            }
        }

        start = kNext;
        prefixSize = nextPrefixSize;
    }

    if (result.m_nalTypes.empty()) {
        result.m_nalTypes = "none";
    }
    result.m_payloadFingerprint = formatFingerprint(hash);
    return result;
}

}
}

#endif