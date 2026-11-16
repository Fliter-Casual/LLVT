// ============================================================
// 文件：UdpFrameAssembler.cpp
// 模块：sclient::transport
// 功能：分片重组的实现：合法性校验 → 位图去重 → 累计 → 凑齐交付；
//       在途记录超时或在超限时被剪枝。
// 知识点：边界校验先于任何内存写入、位图判定完整性、容器容量控制
// ============================================================

#include "sclient/transport/udp/UdpFrameAssembler.h"

#include <algorithm>
#include <cstring>

namespace sclient {
namespace transport {

namespace {

void setBit(std::vector<std::uint8_t> *bits, std::size_t index) {
    (*bits)[index / 8] |= static_cast<std::uint8_t>(1U << (index % 8));
}

bool getBit(const std::vector<std::uint8_t> &bits, std::size_t index) {
    return (bits[index / 8] & static_cast<std::uint8_t>(1U << (index % 8))) != 0;
}

}  // namespace

// 所有校验都在分配内存与写字节之前完成：畸形头部（分片数、偏移、长度
// 互相矛盾）必须先被挡住，否则一次伪造就能造成越界写。
bool UdpFrameAssembler::isFragmentValid(const UdpFrameFragmentHeader &fragment,
                                        std::size_t payloadBytes) const {
    if (fragment.m_fragmentCount == 0) {
        return false;
    }
    if (fragment.m_fragmentIndex >= fragment.m_fragmentCount) {
        return false;
    }
    if (fragment.m_fragmentRole > static_cast<std::uint16_t>(UdpFragmentRole::Retransmission)) {
        return false;
    }
    if (fragment.m_framePayloadSize == 0
        || fragment.m_framePayloadSize > m_options.m_maxFrameBytes) {
        return false;
    }
    if (payloadBytes == 0) {
        return false;
    }
    const std::uint64_t offset = fragment.m_fragmentOffset;
    if (offset + payloadBytes > fragment.m_framePayloadSize) {
        return false;
    }
    return true;
}

bool UdpFrameAssembler::addFragment(const UdpFrameFragmentHeader &fragment,
                                    const std::uint8_t *payload,
                                    std::size_t payloadBytes,
                                    std::uint64_t nowNs,
                                    ReceivedFrame *frame) {
    if (payload == nullptr || frame == nullptr || !isFragmentValid(fragment, payloadBytes)) {
        ++m_stats.m_invalidFragments;
        return false;
    }

    auto iterator = m_assemblies.find(fragment.m_frameSequence);
    if (iterator == m_assemblies.end()) {
        // 新帧：在途记录过多时先剪掉超时的，仍然过多就拒绝这一帧。
        if (m_assemblies.size() >= m_options.m_maxInFlightFrames) {
            prune(nowNs);
            if (m_assemblies.size() >= m_options.m_maxInFlightFrames) {
                ++m_stats.m_invalidFragments;
                return false;
            }
        }
        Assembly fresh;
        fresh.m_metadata = fragment;
        fresh.m_payload.assign(fragment.m_framePayloadSize, 0);
        const std::size_t bitBytes =
                (static_cast<std::size_t>(fragment.m_fragmentCount) + 7) / 8;
        fresh.m_received.assign(bitBytes, 0);
        fresh.m_firstSeenNs = nowNs;
        iterator = m_assemblies.emplace(fragment.m_frameSequence, std::move(fresh)).first;
    }

    Assembly &assembly = iterator->second;
    // 同一帧的分片头必须一致：载荷大小或分片数变了说明序号复用或串包，
    // 继续拼只会拼出四不像。
    if (assembly.m_payload.size() != fragment.m_framePayloadSize
        || assembly.m_received.size()
                != (static_cast<std::size_t>(fragment.m_fragmentCount) + 7) / 8) {
        ++m_stats.m_invalidFragments;
        m_assemblies.erase(iterator);
        return false;
    }

    if (getBit(assembly.m_received, fragment.m_fragmentIndex)) {
        // 重复分片（重传或网络复制）只计数，不算错误。
        ++m_stats.m_duplicateFragments;
        return false;
    }
    setBit(&assembly.m_received, fragment.m_fragmentIndex);
    ++assembly.m_receivedCount;
    std::memcpy(assembly.m_payload.data() + fragment.m_fragmentOffset, payload, payloadBytes);

    if (assembly.m_receivedCount < fragment.m_fragmentCount) {
        return false;
    }

    frame->m_metadata = assembly.m_metadata;
    frame->m_receiveTimestampNs = nowNs;
    frame->m_payload = std::move(assembly.m_payload);
    m_assemblies.erase(iterator);
    ++m_stats.m_completedFrames;
    m_stats.m_completedBytes += frame->m_payload.size();
    return true;
}

void UdpFrameAssembler::prune(std::uint64_t nowNs) {
    for (auto iterator = m_assemblies.begin(); iterator != m_assemblies.end();) {
        const std::uint64_t firstSeen = iterator->second.m_firstSeenNs;
        if (nowNs > firstSeen && nowNs - firstSeen >= m_options.m_assemblyTimeoutNs) {
            iterator = m_assemblies.erase(iterator);
            ++m_stats.m_timedOutFrames;
        } else {
            ++iterator;
        }
    }
}

}  // namespace transport
}  // namespace sclient
