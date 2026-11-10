// ============================================================
// 文件：TokenBucket.h
// 模块：sserver::transport
// 功能：发送整形的令牌桶：按固定速率补充字节额度，突发额度封顶。
//       纯逻辑，不睡眠也不看时钟，时间由调用方传入。
// 知识点：令牌桶模型、突发上限、时间驱动补充、非阻塞取用
// ============================================================

#ifndef SSERVER_TRANSPORT_TOKEN_BUCKET_H
#define SSERVER_TRANSPORT_TOKEN_BUCKET_H

#include <cstddef>
#include <cstdint>

namespace sserver {
namespace transport {

// 为什么需要整形：关键帧一帧几十 KB，一口气全发出去会瞬间打满内核与 WiFi
// 队列，丢的很可能就是这个关键帧。按令牌桶发，突发被摊平成一段可控的流。
//
// 为什么是"取不到就报告要等多久"而不是"内部 sleep"：重传请求跑在收包线程
// 上，它不能为了等额度把收包循环卡住——那里必须"没额度就放弃这一批"。
class TokenBucket {
public:
    // rateBytesPerSecond <= 0 或 burstBytes <= 0 时构造出的是"永远无额度"
    // 的桶；需要关闭整形时不要用它，而是在调用方跳过这一层。
    TokenBucket(double rateBytesPerSecond, double burstBytes);

    // 尝试取走 bytes。
    // @param waitNs 非空时，失败返回还要等多少纳秒才够这一次的量
    //               （按当前速率线性推算，实际会因补充精度略有出入）
    bool tryAcquire(std::size_t bytes, std::uint64_t nowNs, std::uint64_t *waitNs = nullptr);

    // 当前可用额度（字节，向下取整到整数）。
    double availableBytes(std::uint64_t nowNs) const;

    double rateBytesPerSecond() const { return m_rateBytesPerSecond; }
    double burstBytes() const { return m_burstBytes; }

private:
    // 按经过的时间补充额度，并把上限夹在突发额度上：空闲一小时不该攒出
    // 一小时的信用，否则第一次发送仍然是突发。
    void refill(std::uint64_t nowNs) const;

    double m_rateBytesPerSecond;
    double m_burstBytes;
    mutable double m_tokens;
    mutable std::uint64_t m_lastRefillNs;
    mutable bool m_primed = false;
};

}  // namespace transport
}  // namespace sserver

#endif  // SSERVER_TRANSPORT_TOKEN_BUCKET_H
