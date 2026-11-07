// ============================================================
// 文件：StreamProtocol.h
// 模块：sserver::transport
// 功能：UDP 路径的自定义应用层协议：消息类型枚举、魔数、packed 线格式
//       结构体（消息头 / 帧元数据 / 分片头 / 接收报告 / NACK / 保活确认）
//       与显式小端序列化、解析函数。
// 知识点：enum class 底层类型、#pragma pack 与 static_assert 布局校验、
//         显式字节序读写、memcpy 类型双关、inline 与 ODR
// ============================================================
// 基于 UDP，自己写一套应用层协议，用于视频流传输
// ============================================================
#ifndef SSERVER_TRANSPORT_STREAM_PROTOCOL_H
#define SSERVER_TRANSPORT_STREAM_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sserver {
namespace transport {

// ==================== 消息类型与魔数(快速识别垃圾数据) ====================

// 显式定死 16 位底型：枚举值直接就是线上字段值，序列化时不需要二次映射。
enum class MessageType : std::uint16_t {
    KeepAlive = 0,    /**< 客户端保活；UDP 下必须携带完整 UdpReceiverReport */
    VideoStream = 1,  /**< 视频分片；subType 用 base::StreamPayloadType */
    UdpNack = 2,      /**< 分片重传请求；载荷为 UdpNackHeader + N 个 UdpNackItem */
    KeepAliveAck = 3, /**< 保活回显；回显客户端时间戳供其估算 RTT */
};

// 视频分片角色枚举
enum class UdpFragmentRole : std::uint16_t {
    Data = 0,           /**< 首发数据分片 */
    XorParity = 1,      /**< 分组异或校验片：同组数据片补零后逐字节异或 */
    Retransmission = 2, /**< 重传数据；不计入首发丢包统计 */
};

// 所有消息头共用的魔数。收到一个不认识的数据报时，先看这四个字节就能
// 直接把"不是本协议的包"和"本协议的畸形包"区分开。
static const char kMessageMagic[4] = {'C', 'C', 'T', 'D'};

// ==================== 线格式结构体 ====================

// pack(1) 消除成员间填充，让 sizeof 与线上字节数严格一致；push/pop 保存
// 并恢复调用方的对齐设置，不影响包含本头文件的其它代码。
//
// 注意：这些结构体只是主机内存里的载体。绝不允许直接 memcpy 到线上或
// 从线上 memcpy 进来——Wi-Fi 抓包、大小端主机、结构体布局演化任何一项
// 都会让这种写法静默错位。线上读写一律走下面的显式小端函数。
#pragma pack(push, 1) // 将当前对齐方式压栈保存，并临时把结构体成员的对齐字节数设置为 1 字节

// 12 字节固定头：魔数 4 + 类型 2 + 子类型 2 + 载荷长度 4。
// 每个数据报（分片 / 保活 / NACK）都以它开头。
struct MessageHeader {
    char m_headId[4]; // 消息头魔数
    std::uint16_t m_messageType; // 消息类型
    std::uint16_t m_subType; // 子类型
    std::uint32_t m_payloadLength; // 不含本头的字节数
};

// 40 字节诊断元数据：帧序号加四个纳秒时间戳，覆盖采集→编码开始→编码结束→
// 发送。客户端的分段延迟统计全靠这几个时间戳，少一个就归因不了。
struct FrameDiagnosticMetadata {
    std::uint64_t m_sequence; // 帧序号
    std::uint64_t m_captureTimestampNs; // 采集时间戳，纳秒级
    std::uint64_t m_encodeStartTimestampNs; // 编码开始时间戳，纳秒级
    std::uint64_t m_encodeEndTimestampNs; // 编码结束时间戳，纳秒级
    std::uint64_t m_transportSendTimestampNs; // 发送时间戳，纳秒级
};

// 84 字节分片头：前 40 字节与 FrameDiagnosticMetadata 同构（接收端可以直接
// 拿同一段布局解析时间戳）。frameSequence + fragmentIndex/Offset 构成乱序
// 重组的定位三元组，缺任何一个都无法把分片拼回帧。
struct UdpFrameFragmentHeader {
    std::uint64_t m_frameSequence;
    std::uint64_t m_captureTimestampNs;
    std::uint64_t m_encodeStartTimestampNs;
    std::uint64_t m_encodeEndTimestampNs;
    std::uint64_t m_transportSendTimestampNs;
    std::uint32_t m_framePayloadSize;   /**< 整帧载荷总长，各分片相同 */
    std::uint32_t m_fragmentOffset;     /**< 本分片在帧载荷内的偏移 */
    std::uint16_t m_fragmentIndex;      /**< 本分片在帧内的序号，从 0 开始 */
    std::uint16_t m_fragmentCount;      /**< 帧内分片总数 */
    std::uint16_t m_fragmentRole;       /**< UdpFragmentRole 值 */
    std::uint16_t m_parityGroupIndex;   /**< 交织 XOR 分组号：数据片填 index % 组数 */
    std::uint64_t m_originalPacketSequence; /**< 首发序号；校验片与重传填 0 */
    std::uint64_t m_packetSendTimestampNs;  /**< 过了 pacing 的逐包发送时刻 */
    std::uint64_t m_transportFrameIndex;    /**< 进入发送路径的帧序号，与编码序号可能不同 */
    std::uint32_t m_frameFlags;             /**< bit0 = 关键帧 */
};

// 最大反馈样本数(constexpr 变量必须在编译期完成初始化。所有 constexpr 变量都隐含 const 属性，不可修改)
constexpr std::size_t kUdpPacketFeedbackCapacity = 16;

// 单个首发包的观测样本。发送与到达时间戳分属两端时钟域，只能各自求间隔，
// 不能直接相减当单向时延。(UDP 数据包反馈)
struct UdpPacketFeedback {
    std::uint64_t m_sequence; // 首发序号
    std::uint64_t m_sendTimestampNs; // 发送时间戳，纳秒级
    std::uint64_t m_arrivalTimestampNs; // 到达时间戳，纳秒级
    std::uint32_t m_wireBytes; // 线路字节数
};

// 660 字节接收报告：客户端统计快照，搭乘 keepalive 回传，服务端据此观测
// 该端点的丢包、乱序与恢复效果，也是 UDP 地址注册的载体（服务端从合法报告里学习该客户端的收端地址）。
struct UdpReceiverReport {
    std::uint64_t m_reportTimestampNs; // 报告时间戳，纳秒级
    std::uint64_t m_datagramsReceived; // 接收的数据报数
    std::uint64_t m_invalidDatagrams; // 无效数据报数
    std::uint64_t m_fragmentsReceived; // 接收的分片数
    std::uint64_t m_duplicateFragments; // 重复分片数
    std::uint64_t m_timedOutFragments; // 超时分片数
    std::uint64_t m_timedOutFrames; // 超时帧数
    std::uint64_t m_completedFrames; // 完成的帧数
    std::uint64_t m_reorderedFrames; // 乱序帧数
    std::uint64_t m_jitterSamples; // 时延样本数
    double m_jitterLastMs; // 最近一次时延，毫秒级
    double m_jitterAvgMs; // 平均时延，毫秒级
    double m_jitterMaxMs; // 最大时延，毫秒级
    // 以下为网络恢复与质量统计，随完整报告一起发。
    std::uint64_t m_rttNs;                 /**< 平滑 RTT；0 表示未知 */
    double m_jitterP95Ms; // 95% 时延，毫秒级
    std::uint64_t m_fecRecoveredFragments; // 从 FEC 恢复的分片数
    std::uint64_t m_fecRecoveredFrames; // 从 FEC 恢复的帧数
    std::uint64_t m_nackRequestsSent; // 发送的 NACK 请求数
    // 首发包单独结算丢失：FEC 与重传不进分母，否则"修好了"会拉低真实丢包率。
    std::uint64_t m_originalPacketsExpected; // 预期的首发包数
    std::uint64_t m_originalPacketsLost; // 丢失的首发包数
    std::uint64_t m_originalPacketsReceived; // 接收的首发包数
    std::uint64_t m_transportFramesExpected; // 预期的帧数
    std::uint64_t m_transportFramesLost; // 丢失的帧数
    std::uint64_t m_retransmittedFragmentsReceived; // 重传的分片数
    std::uint64_t m_nackRecoveredFrames; // 从 NACK 恢复的帧数
    std::uint64_t m_receiveRateBps; // 接收速率，bps
    std::uint32_t m_packetFeedbackCount; // 数据包反馈样本数
    UdpPacketFeedback m_packetFeedback[kUdpPacketFeedbackCapacity]; // 数据包反馈样本
};

// 选择性重传：头 + 变长 UdpNackItem 数组。只点名缺失分片而非整帧重发，
// 重传开销与丢包率成正比，而不是与帧大小成正比。
struct UdpNackHeader {
    std::uint64_t m_requestTimestampNs; // 请求时间戳，纳秒级
    std::uint16_t m_requestCount; // 请求次数
    std::uint16_t m_reserved; // 保留字段
};

// 重传项：指定缺失的分片序号。
struct UdpNackItem {
    std::uint64_t m_frameSequence; // 帧序号
    std::uint16_t m_fragmentIndex; // 分片序号
    std::uint16_t m_reserved; // 保留字段
};

// 保活确认：原样回显客户端报告时间戳，客户端用「现在 − 回显值」估 RTT。
struct UdpKeepAliveAck {
    std::uint64_t m_clientReportTimestampNs; // 客户端报告时间戳
    std::uint64_t m_serverTimestampNs; // 服务器时间戳
};

#pragma pack(pop)

// ==================== 魔数辅助 ====================

// char(&)[4] 形参在编译期钉死长度，传错数组大小直接编译失败。
// 用于填充消息头的魔数，确保消息头的魔数与协议规范一致。
inline void fillMessageMagic(char (&buffer)[4]) {
    std::memcpy(buffer, kMessageMagic, sizeof(kMessageMagic));
}

// 用于校验消息头的魔数是否与协议规范一致。
inline bool hasValidMessageMagic(const MessageHeader &header) {
    return std::memcmp(header.m_headId, kMessageMagic, sizeof(kMessageMagic)) == 0;
}

// ==================== 线格式尺寸 ====================

constexpr std::size_t kMessageHeaderWireSize = 12;
constexpr std::size_t kFrameDiagnosticMetadataWireSize = 40;
constexpr std::size_t kUdpFrameFragmentHeaderWireSize = 84;
constexpr std::size_t kUdpReceiverReportWireSize = 660;
constexpr std::size_t kUdpKeepAliveAckWireSize = 16;
constexpr std::size_t kUdpNackHeaderWireSize = 12;
constexpr std::size_t kUdpNackItemWireSize = 12;

// 编译期钉死布局：将来谁在结构体中间插一个字段、或者换了编译器内置对齐，
// 这里立刻编译失败，而不是等到联调时抓包发现"两端差 4 个字节"。
static_assert(sizeof(MessageHeader) == kMessageHeaderWireSize, "wire layout mismatch");
static_assert(sizeof(FrameDiagnosticMetadata) == kFrameDiagnosticMetadataWireSize,
              "wire layout mismatch");
static_assert(sizeof(UdpFrameFragmentHeader) == kUdpFrameFragmentHeaderWireSize,
              "wire layout mismatch");
static_assert(sizeof(UdpReceiverReport) == kUdpReceiverReportWireSize, "wire layout mismatch");
static_assert(sizeof(UdpKeepAliveAck) == kUdpKeepAliveAckWireSize, "wire layout mismatch");
static_assert(sizeof(UdpNackHeader) == kUdpNackHeaderWireSize, "wire layout mismatch");
static_assert(sizeof(UdpNackItem) == kUdpNackItemWireSize, "wire layout mismatch");

// ==================== 显式小端原语 ====================

// 逐字节移位拼装，结果与主机字节序无关。这些函数不校验长度也不做语义
// 检查，调用方必须保证缓冲区足够大。
// 用于从字节数组中读取小端序的 16 位无符号整数。
inline std::uint16_t readLe16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0])
                                      | (static_cast<std::uint16_t>(data[1]) << 8));
}

// 用于从字节数组中读取小端序的 32 位无符号整数。
inline std::uint32_t readLe32(const std::uint8_t *data) {
    return static_cast<std::uint32_t>(data[0])
            | (static_cast<std::uint32_t>(data[1]) << 8)
            | (static_cast<std::uint32_t>(data[2]) << 16)
            | (static_cast<std::uint32_t>(data[3]) << 24);
}

// 用于从字节数组中读取小端序的 64 位无符号整数。
inline std::uint64_t readLe64(const std::uint8_t *data) {
    return static_cast<std::uint64_t>(readLe32(data))
            | (static_cast<std::uint64_t>(readLe32(data + 4)) << 32);
}

// 用于将 16 位无符号整数写入字节数组中，小端序。
inline void writeLe16(std::uint16_t value, std::uint8_t *data) {
    data[0] = static_cast<std::uint8_t>(value & 0xFFU);
    data[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
}

// 用于将 32 位无符号整数写入字节数组中，小端序。
inline void writeLe32(std::uint32_t value, std::uint8_t *data) {
    data[0] = static_cast<std::uint8_t>(value & 0xFFU);
    data[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    data[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
    data[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
}

// 用于将 64 位无符号整数写入字节数组中，小端序。
inline void writeLe64(std::uint64_t value, std::uint8_t *data) {
    writeLe32(static_cast<std::uint32_t>(value & 0xFFFFFFFFULL), data);
    writeLe32(static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFULL), data + 4);
}

// double 走位表示传输：经 memcpy 复制到 uint64 是标准允许的别名安全写法，
// 直接用 reinterpret_cast 属于未定义行为。
inline std::uint64_t doubleToWireBits(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// 用于将 uint64 转换为 double。
inline double wireBitsToDouble(std::uint64_t bits) {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// ==================== 序列化 / 解析 ====================

// 每个 writeLe 的偏移就是该字段的线上位置。发送与接收两侧共用同一组
// 偏移量，天然互为镜像；改任何一个偏移都必须同时改两端和黄金样本测试。
// 用于序列化消息头到字节数组中。
inline void serializeMessageHeader(const MessageHeader &header, std::uint8_t *out) {
    std::memcpy(out, kMessageMagic, sizeof(kMessageMagic));
    writeLe16(static_cast<std::uint16_t>(header.m_messageType), out + 4);
    writeLe16(static_cast<std::uint16_t>(header.m_subType), out + 6);
    writeLe32(header.m_payloadLength, out + 8);
}

// 唯一检查了长度的解析入口：数据报长度得先够一个头，才能谈内容。
// 用于解析字节数组中的消息头到 MessageHeader 结构体中
inline bool parseMessageHeader(const std::uint8_t *data, std::size_t size,
                               MessageHeader *header) {
    if (header == nullptr || data == nullptr || size < kMessageHeaderWireSize) {
        return false;
    }
    std::memcpy(header->m_headId, data, sizeof(kMessageMagic));
    header->m_messageType = readLe16(data + 4);
    header->m_subType = readLe16(data + 6);
    header->m_payloadLength = readLe32(data + 8);
    return true;
}

// 用于序列化 FrameDiagnosticMetadata 到字节数组中。
inline void serializeFrameDiagnosticMetadata(const FrameDiagnosticMetadata &metadata,
                                             std::uint8_t *out) {
    writeLe64(metadata.m_sequence, out + 0);
    writeLe64(metadata.m_captureTimestampNs, out + 8);
    writeLe64(metadata.m_encodeStartTimestampNs, out + 16);
    writeLe64(metadata.m_encodeEndTimestampNs, out + 24);
    writeLe64(metadata.m_transportSendTimestampNs, out + 32);
}

// 用于解析字节数组中的 FrameDiagnosticMetadata 到 FrameDiagnosticMetadata 结构体中。
inline void parseFrameDiagnosticMetadata(const std::uint8_t *data,
                                         FrameDiagnosticMetadata *metadata) {
    metadata->m_sequence = readLe64(data + 0);
    metadata->m_captureTimestampNs = readLe64(data + 8);
    metadata->m_encodeStartTimestampNs = readLe64(data + 16);
    metadata->m_encodeEndTimestampNs = readLe64(data + 24);
    metadata->m_transportSendTimestampNs = readLe64(data + 32);
}

// 用于序列化 UdpFrameFragmentHeader 到字节数组中。
inline void serializeUdpFrameFragmentHeader(const UdpFrameFragmentHeader &header,
                                            std::uint8_t *out) {
    writeLe64(header.m_frameSequence, out + 0);
    writeLe64(header.m_captureTimestampNs, out + 8);
    writeLe64(header.m_encodeStartTimestampNs, out + 16);
    writeLe64(header.m_encodeEndTimestampNs, out + 24);
    writeLe64(header.m_transportSendTimestampNs, out + 32);
    writeLe32(header.m_framePayloadSize, out + 40);
    writeLe32(header.m_fragmentOffset, out + 44);
    writeLe16(header.m_fragmentIndex, out + 48);
    writeLe16(header.m_fragmentCount, out + 50);
    writeLe16(header.m_fragmentRole, out + 52);
    writeLe16(header.m_parityGroupIndex, out + 54);
    writeLe64(header.m_originalPacketSequence, out + 56);
    writeLe64(header.m_packetSendTimestampNs, out + 64);
    writeLe64(header.m_transportFrameIndex, out + 72);
    writeLe32(header.m_frameFlags, out + 80);
}

// 用于解析字节数组中的 UdpFrameFragmentHeader 到 UdpFrameFragmentHeader 结构体中。
inline void parseUdpFrameFragmentHeader(const std::uint8_t *data,
                                        UdpFrameFragmentHeader *header) {
    header->m_frameSequence = readLe64(data + 0);
    header->m_captureTimestampNs = readLe64(data + 8);
    header->m_encodeStartTimestampNs = readLe64(data + 16);
    header->m_encodeEndTimestampNs = readLe64(data + 24);
    header->m_transportSendTimestampNs = readLe64(data + 32);
    header->m_framePayloadSize = readLe32(data + 40);
    header->m_fragmentOffset = readLe32(data + 44);
    header->m_fragmentIndex = readLe16(data + 48);
    header->m_fragmentCount = readLe16(data + 50);
    header->m_fragmentRole = readLe16(data + 52);
    header->m_parityGroupIndex = readLe16(data + 54);
    header->m_originalPacketSequence = readLe64(data + 56);
    header->m_packetSendTimestampNs = readLe64(data + 64);
    header->m_transportFrameIndex = readLe64(data + 72);
    header->m_frameFlags = readLe32(data + 80);
}

// 接收报告：前 208 字节是计数与抖动指标，之后是 4 字节样本数 + 16 个
// 28 字节样本（16 × 28 = 448，208 + 4 + 448 = 660）。
inline void serializeUdpReceiverReport(const UdpReceiverReport &report, std::uint8_t *out) {
    writeLe64(report.m_reportTimestampNs, out + 0);
    writeLe64(report.m_datagramsReceived, out + 8);
    writeLe64(report.m_invalidDatagrams, out + 16);
    writeLe64(report.m_fragmentsReceived, out + 24);
    writeLe64(report.m_duplicateFragments, out + 32);
    writeLe64(report.m_timedOutFragments, out + 40);
    writeLe64(report.m_timedOutFrames, out + 48);
    writeLe64(report.m_completedFrames, out + 56);
    writeLe64(report.m_reorderedFrames, out + 64);
    writeLe64(report.m_jitterSamples, out + 72);
    writeLe64(doubleToWireBits(report.m_jitterLastMs), out + 80);
    writeLe64(doubleToWireBits(report.m_jitterAvgMs), out + 88);
    writeLe64(doubleToWireBits(report.m_jitterMaxMs), out + 96);
    writeLe64(report.m_rttNs, out + 104);
    writeLe64(doubleToWireBits(report.m_jitterP95Ms), out + 112);
    writeLe64(report.m_fecRecoveredFragments, out + 120);
    writeLe64(report.m_fecRecoveredFrames, out + 128);
    writeLe64(report.m_nackRequestsSent, out + 136);
    writeLe64(report.m_originalPacketsExpected, out + 144);
    writeLe64(report.m_originalPacketsLost, out + 152);
    writeLe64(report.m_originalPacketsReceived, out + 160);
    writeLe64(report.m_transportFramesExpected, out + 168);
    writeLe64(report.m_transportFramesLost, out + 176);
    writeLe64(report.m_retransmittedFragmentsReceived, out + 184);
    writeLe64(report.m_nackRecoveredFrames, out + 192);
    writeLe64(report.m_receiveRateBps, out + 200);
    writeLe32(report.m_packetFeedbackCount, out + 208);
    for (std::size_t index = 0; index < kUdpPacketFeedbackCapacity; ++index) {
        const UdpPacketFeedback &sample = report.m_packetFeedback[index];
        std::uint8_t *sampleOut = out + 212 + index * 28;
        writeLe64(sample.m_sequence, sampleOut);
        writeLe64(sample.m_sendTimestampNs, sampleOut + 8);
        writeLe64(sample.m_arrivalTimestampNs, sampleOut + 16);
        writeLe32(sample.m_wireBytes, sampleOut + 24);
    }
}

inline void parseUdpReceiverReport(const std::uint8_t *data, UdpReceiverReport *report) {
    report->m_reportTimestampNs = readLe64(data + 0);
    report->m_datagramsReceived = readLe64(data + 8);
    report->m_invalidDatagrams = readLe64(data + 16);
    report->m_fragmentsReceived = readLe64(data + 24);
    report->m_duplicateFragments = readLe64(data + 32);
    report->m_timedOutFragments = readLe64(data + 40);
    report->m_timedOutFrames = readLe64(data + 48);
    report->m_completedFrames = readLe64(data + 56);
    report->m_reorderedFrames = readLe64(data + 64);
    report->m_jitterSamples = readLe64(data + 72);
    report->m_jitterLastMs = wireBitsToDouble(readLe64(data + 80));
    report->m_jitterAvgMs = wireBitsToDouble(readLe64(data + 88));
    report->m_jitterMaxMs = wireBitsToDouble(readLe64(data + 96));
    report->m_rttNs = readLe64(data + 104);
    report->m_jitterP95Ms = wireBitsToDouble(readLe64(data + 112));
    report->m_fecRecoveredFragments = readLe64(data + 120);
    report->m_fecRecoveredFrames = readLe64(data + 128);
    report->m_nackRequestsSent = readLe64(data + 136);
    report->m_originalPacketsExpected = readLe64(data + 144);
    report->m_originalPacketsLost = readLe64(data + 152);
    report->m_originalPacketsReceived = readLe64(data + 160);
    report->m_transportFramesExpected = readLe64(data + 168);
    report->m_transportFramesLost = readLe64(data + 176);
    report->m_retransmittedFragmentsReceived = readLe64(data + 184);
    report->m_nackRecoveredFrames = readLe64(data + 192);
    report->m_receiveRateBps = readLe64(data + 200);
    report->m_packetFeedbackCount = readLe32(data + 208);
    for (std::size_t index = 0; index < kUdpPacketFeedbackCapacity; ++index) {
        UdpPacketFeedback &sample = report->m_packetFeedback[index];
        const std::uint8_t *sampleIn = data + 212 + index * 28;
        sample.m_sequence = readLe64(sampleIn);
        sample.m_sendTimestampNs = readLe64(sampleIn + 8);
        sample.m_arrivalTimestampNs = readLe64(sampleIn + 16);
        sample.m_wireBytes = readLe32(sampleIn + 24);
    }
}
// 用于序列化 UdpKeepAliveAck 到字节数组中。
inline void serializeUdpKeepAliveAck(const UdpKeepAliveAck &ack, std::uint8_t *out) {
    writeLe64(ack.m_clientReportTimestampNs, out + 0);
    writeLe64(ack.m_serverTimestampNs, out + 8);
}
// 用于解析字节数组中的 UdpKeepAliveAck 到 UdpKeepAliveAck 结构体中。
inline void parseUdpKeepAliveAck(const std::uint8_t *data, UdpKeepAliveAck *ack) {
    ack->m_clientReportTimestampNs = readLe64(data + 0);
    ack->m_serverTimestampNs = readLe64(data + 8);
}
// 用于序列化 UdpNackHeader 到字节数组中。
inline void serializeUdpNackHeader(const UdpNackHeader &header, std::uint8_t *out) {
    writeLe64(header.m_requestTimestampNs, out + 0);
    writeLe16(header.m_requestCount, out + 8);
    writeLe16(header.m_reserved, out + 10);
}
// 用于解析字节数组中的 UdpNackHeader 到 UdpNackHeader 结构体中。
inline void parseUdpNackHeader(const std::uint8_t *data, UdpNackHeader *header) {
    header->m_requestTimestampNs = readLe64(data + 0);
    header->m_requestCount = readLe16(data + 8);
    header->m_reserved = readLe16(data + 10);
}
// 用于序列化 UdpNackItem 到字节数组中。
inline void serializeUdpNackItem(const UdpNackItem &item, std::uint8_t *out) {
    writeLe64(item.m_frameSequence, out + 0);
    writeLe16(item.m_fragmentIndex, out + 8);
    writeLe16(item.m_reserved, out + 10);
}
// 用于解析字节数组中的 UdpNackItem 到 UdpNackItem 结构体中。
inline void parseUdpNackItem(const std::uint8_t *data, UdpNackItem *item) {
    item->m_frameSequence = readLe64(data + 0);
    item->m_fragmentIndex = readLe16(data + 8);
    item->m_reserved = readLe16(data + 10);
}

}  // namespace transport
}  // namespace sserver

#endif  // SSERVER_TRANSPORT_STREAM_PROTOCOL_H
