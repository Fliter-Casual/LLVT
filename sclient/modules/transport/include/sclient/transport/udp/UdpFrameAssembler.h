// ============================================================
// 文件：UdpFrameAssembler.h
// 模块：sclient::transport
// 功能：把乱序到达的分片按帧号重组：记录每帧已收到的分片位图，凑齐后交付
//       完整帧；超时未凑齐的记录定期剪枝，防止坏帧堆积。
// 知识点：位图去重与完整性判定、乱序处理、在途帧上限、超时剪枝
// ============================================================

#ifndef SCLIENT_TRANSPORT_UDP_FRAME_ASSEMBLER_H
#define SCLIENT_TRANSPORT_UDP_FRAME_ASSEMBLER_H

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sclient/transport/ReceivedFrame.h"
#include "sclient/transport/protocol/StreamProtocol.h"

namespace sclient {
namespace transport {

// 纯逻辑：不持有 socket、不看墙钟（时间由调用方传入），因此可以脱离网络
// 精确测试乱序、重复、缺片与超时这四类情形。
class UdpFrameAssembler {
public:
    struct Options {
        // 单帧载荷上限。超过就判定为畸形数据报：不设上限意味着一个伪造的
        // 头部就能让客户端分配几十 MB。
        std::size_t m_maxFrameBytes = 4U * 1024U * 1024U;
        // 未凑齐的帧保留多久。超过就剪枝——半截帧没有任何用处，
        // 留着只会占内存并让"等这一帧"的判断永远成立。
        std::uint64_t m_assemblyTimeoutNs = 250ULL * 1000ULL * 1000ULL;
        // 同时在途的帧数上限，防止丢包严重时记录表无限增长。
        std::size_t m_maxInFlightFrames = 64;
    };

    struct Stats {
        std::uint64_t m_completedFrames = 0;
        std::uint64_t m_duplicateFragments = 0;
        std::uint64_t m_invalidFragments = 0;
        std::uint64_t m_timedOutFrames = 0;
        std::uint64_t m_completedBytes = 0;
    };

    UdpFrameAssembler() = default;
    explicit UdpFrameAssembler(const Options &options) : m_options(options) {}

    // 喂入一个分片。凑齐整帧时返回 true 并填充 frame（此时记录已移除）。
    bool addFragment(const UdpFrameFragmentHeader &fragment,
                     const std::uint8_t *payload,
                     std::size_t payloadBytes,
                     std::uint64_t nowNs,
                     ReceivedFrame *frame);

    // 剪掉超时未凑齐的帧。收包循环只要定期调用即可（例如每 10ms）。
    void prune(std::uint64_t nowNs);

    std::size_t inFlightFrames() const { return m_assemblies.size(); }
    Stats stats() const { return m_stats; }

private:
    struct Assembly {
        UdpFrameFragmentHeader m_metadata;   // 首片头部
        std::vector<std::uint8_t> m_payload;
        std::vector<std::uint8_t> m_received; // 每位对应一个分片
        std::size_t m_receivedCount = 0;
        std::uint64_t m_firstSeenNs = 0;
    };

    bool isFragmentValid(const UdpFrameFragmentHeader &fragment, std::size_t payloadBytes) const;

    Options m_options;
    std::unordered_map<std::uint64_t, Assembly> m_assemblies;
    Stats m_stats;
};

}  // namespace transport
}  // namespace sclient

#endif  // SCLIENT_TRANSPORT_UDP_FRAME_ASSEMBLER_H
