// ============================================================
// 文件：StreamProducer.h
// 模块：sserver::pipeline
// 功能：一个采集组的最小媒体生产管线：采集线程 → 原始帧队列 → 编码线程
//       → 编码帧队列 → 分发线程 → 回调。源与编码器都由外部注入。
// 知识点：三级队列逐级收窄、丢帧计数、关键帧补偿信号、优雅停止与排空
// ============================================================

#ifndef SSERVER_PIPELINE_STREAM_PRODUCER_H
#define SSERVER_PIPELINE_STREAM_PRODUCER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "sserver/appconfig/CodecConfig.h"
#include "sserver/base/EncodedFrame.h"
#include "sserver/base/KeyframeRecoverySignal.h"
#include "sserver/base/ThreadSafeQueue.h"
#include "sserver/capture/FrameSource.h"
#include "sserver/encoding/Encoder.h"

namespace sserver {
namespace pipeline {

// 分发回调：app 在这里把帧扇出到各会话的传输信箱。回调必须快速返回，
// 不允许阻塞等待网络——慢的会话只应该在自己那一层丢帧。
using EncodedFrameSink = std::function<void(base::EncodedFramePtr)>;

struct StreamProducerConfig {
    // 队列深度逐级收窄：采集侧容忍一点抖动，接近出口就必须尽量浅。
    std::size_t m_rawQueueDepth = 3;
    std::size_t m_encodedQueueDepth = 2;
    // 采集源暂时无帧时的重试间隔。
    std::uint64_t m_idleRetryNs = 1000000ULL;
};

// 为什么采集与编码必须分成两条线程：摄像头按驱动协商的帧率匀速出帧，
// 编码耗时是抖动的。直接调用意味着编码慢的时候反压采集线程，驱动缓冲
// 耗尽后开始丢帧，丢在哪一层完全不可控。
class StreamProducer {
public:
    struct Stats {
        std::uint64_t m_encodedFrames = 0;
        std::uint64_t m_droppedRawFrames = 0;      /**< 采集队列满丢最旧 */
        std::uint64_t m_droppedEncodedFrames = 0;  /**< 编码队列满丢最旧（会触发补关键帧） */
        std::uint64_t m_encodeErrors = 0;
    };

    StreamProducer(std::unique_ptr<capture::FrameSource> source,
                   std::unique_ptr<encoding::Encoder> encoder,
                   appconfig::CodecConfig codecConfig,
                   StreamProducerConfig config = StreamProducerConfig());
    ~StreamProducer();

    StreamProducer(const StreamProducer &) = delete;
    StreamProducer &operator=(const StreamProducer &) = delete;

    // 打开并启动采集源与编码器，然后拉起三条线程。
    bool start(EncodedFrameSink sink, std::string *errorMessage);

    // 停止并排空：先让各线程把队列里剩下的处理完，再回收线程。
    // 幂等，可重复调用。
    void stop();

    bool running() const { return m_running.load(std::memory_order_acquire); }

    // 转发一次关键帧请求给编码器（控制面的 KEYFRAME 命令最终落到这里）。
    void requestKeyframe();

    Stats stats() const;

    int width() const { return m_width; }
    int height() const { return m_height; }
    int fps() const { return m_fps; }

private:
    void captureLoop();
    void encodeLoop();
    void dispatchLoop();

    std::unique_ptr<capture::FrameSource> m_source;
    std::unique_ptr<encoding::Encoder> m_encoder;
    appconfig::CodecConfig m_codecConfig;
    StreamProducerConfig m_config;

    base::ThreadSafeQueue<capture::RawCaptureFramePtr> m_rawQueue;
    base::ThreadSafeQueue<base::EncodedFramePtr> m_encodedQueue;
    base::KeyframeRecoverySignal m_keyframeSignal;

    EncodedFrameSink m_sink;
    std::atomic<bool> m_running{false};
    std::thread m_captureThread;
    std::thread m_encodeThread;
    std::thread m_dispatchThread;

    std::uint64_t m_nextSequence = 0;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;

    std::atomic<std::uint64_t> m_encodedFrames{0};
    std::atomic<std::uint64_t> m_droppedRawFrames{0};
    std::atomic<std::uint64_t> m_droppedEncodedFrames{0};
    std::atomic<std::uint64_t> m_encodeErrors{0};
};

}  // namespace pipeline
}  // namespace sserver

#endif  // SSERVER_PIPELINE_STREAM_PRODUCER_H
