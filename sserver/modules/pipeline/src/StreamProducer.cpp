// ============================================================
// 文件：StreamProducer.cpp
// 模块：sserver::pipeline
// 功能：三级流水线的线程编排：采集泵、编码泵、分发泵；队列满时丢最旧，
//       丢已编码帧时请求补关键帧；停止时先排空再回收线程。
// 知识点：生产者消费者线程、限时阻塞与停止判据、丢帧后果差异、
//         停止顺序（先分发后编码再采集）
// ============================================================

#include "sserver/pipeline/StreamProducer.h"

#include <chrono>
#include <utility>

#include "sserver/base/MonotonicClock.h"

namespace sserver {
namespace pipeline {

namespace {

constexpr std::uint64_t kQueueWaitMs = 20;
// 采集源协商失败时的兜底尺寸，保证编码器至少有合法参数初始化。
constexpr int kFallbackWidth = 640;
constexpr int kFallbackHeight = 480;
constexpr int kFallbackFps = 25;

}  // namespace

StreamProducer::StreamProducer(std::unique_ptr<capture::FrameSource> source,
                               std::unique_ptr<encoding::Encoder> encoder,
                               appconfig::CodecConfig codecConfig,
                               StreamProducerConfig config)
        : m_source(std::move(source)),
          m_encoder(std::move(encoder)),
          m_codecConfig(std::move(codecConfig)),
          m_config(config) {
}

StreamProducer::~StreamProducer() {
    stop();
}

bool StreamProducer::start(EncodedFrameSink sink, std::string *errorMessage) {
    if (m_running.load()) {
        if (errorMessage != nullptr) {
            *errorMessage = "producer already running";
        }
        return false;
    }
    if (m_source == nullptr || m_encoder == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "producer requires both a frame source and an encoder";
        }
        return false;
    }
    if (m_config.m_rawQueueDepth == 0 || m_config.m_encodedQueueDepth == 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "queue depths must be greater than zero";
        }
        return false;
    }
    if (!sink) {
        if (errorMessage != nullptr) {
            *errorMessage = "producer requires a frame sink";
        }
        return false;
    }

    if (!m_source->open()) {
        if (errorMessage != nullptr) {
            *errorMessage = "failed to open frame source: " + m_source->describe();
        }
        return false;
    }
    if (!m_source->start()) {
        m_source->close();
        if (errorMessage != nullptr) {
            *errorMessage = "failed to start frame source: " + m_source->describe();
        }
        return false;
    }

    // 尺寸与帧率以驱动协商结果为准：码控是按"每帧预算"换算的，
    // 用请求帧率初始化会让码率系统性跑偏。
    m_width = m_source->effectiveWidth() > 0 ? m_source->effectiveWidth() : kFallbackWidth;
    m_height = m_source->effectiveHeight() > 0 ? m_source->effectiveHeight() : kFallbackHeight;
    m_fps = m_source->effectiveFps() > 0 ? m_source->effectiveFps() : kFallbackFps;

    if (!m_encoder->initialize(m_width, m_height, m_fps, m_codecConfig, errorMessage)) {
        m_source->stop();
        m_source->close();
        return false;
    }

    m_sink = std::move(sink);
    m_nextSequence = 0;
    m_running.store(true);
    m_captureThread = std::thread(&StreamProducer::captureLoop, this);
    m_encodeThread = std::thread(&StreamProducer::encodeLoop, this);
    m_dispatchThread = std::thread(&StreamProducer::dispatchLoop, this);
    return true;
}

void StreamProducer::stop() {
    if (!m_running.exchange(false)) {
        // 已经在停止或从未启动；仍然清理一次线程句柄以防上次异常。
        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }
        if (m_encodeThread.joinable()) {
            m_encodeThread.join();
        }
        if (m_dispatchThread.joinable()) {
            m_dispatchThread.join();
        }
        return;
    }
    // 广播唤醒所有阻塞在队列上的线程，让它们看到停止标志。
    m_rawQueue.notifyAll();
    m_encodedQueue.notifyAll();

    // 停止顺序：采集 → 编码 → 分发。
    // 采集先停，保证不再有新原始帧；编码再停，保证不再有新编码帧；
    // 分发最后停，它的退出路径会先排空队列——这样已经编码好的帧能全部
    // 交付，而不会被遗留在队列里跟随对象一起销毁。
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }
    if (m_encodeThread.joinable()) {
        m_encodeThread.join();
    }
    if (m_dispatchThread.joinable()) {
        m_dispatchThread.join();
    }

    if (m_encoder != nullptr) {
        m_encoder->shutdown();
    }
    if (m_source != nullptr) {
        m_source->stop();
        m_source->close();
    }
}

void StreamProducer::requestKeyframe() {
    m_keyframeSignal.request();
}

StreamProducer::Stats StreamProducer::stats() const {
    Stats result;
    result.m_encodedFrames = m_encodedFrames.load(std::memory_order_relaxed);
    result.m_droppedRawFrames = m_droppedRawFrames.load(std::memory_order_relaxed);
    result.m_droppedEncodedFrames = m_droppedEncodedFrames.load(std::memory_order_relaxed);
    result.m_encodeErrors = m_encodeErrors.load(std::memory_order_relaxed);
    return result;
}

void StreamProducer::captureLoop() {
    while (m_running.load(std::memory_order_acquire)) {
        capture::RawCaptureFramePtr frame = m_source->captureRawFrame();
        if (frame == nullptr) {
            // 没有帧不是错误：刚开流的窗口期、或驱动的超时窗口都会出现。
            std::this_thread::sleep_for(std::chrono::nanoseconds(m_config.m_idleRetryNs));
            continue;
        }
        const std::size_t dropped =
                m_rawQueue.pushDropOldestCountDropped(frame, m_config.m_rawQueueDepth);
        if (dropped > 0) {
            m_droppedRawFrames.fetch_add(dropped, std::memory_order_relaxed);
        }
    }
    // 停止后把采集队列里剩下的交给编码线程处理完。
    m_rawQueue.notifyAll();
}

void StreamProducer::encodeLoop() {
    using namespace std::chrono;
    while (m_running.load(std::memory_order_acquire)) {
        capture::RawCaptureFramePtr raw;
        if (!m_rawQueue.waitPopFor(&raw, milliseconds(kQueueWaitMs))) {
            continue;
        }
        if (m_keyframeSignal.consume(base::monotonicNowNs())) {
            m_encoder->forceKeyframe();
        }

        const base::RawVideoFrameView view = raw->view();
        std::vector<std::uint8_t> payload;
        bool isKeyframe = false;
        const std::uint64_t encodeStartNs = base::monotonicNowNs();
        std::string error;
        if (!m_encoder->encodeFrame(view, &payload, &isKeyframe, &error)) {
            // 单帧编码失败不影响后续帧：丢这一帧整条链已经断了，
            // 用补偿信号让编码器尽快补一个关键帧。
            m_encodeErrors.fetch_add(1, std::memory_order_relaxed);
            m_keyframeSignal.request();
            continue;
        }
        if (payload.empty()) {
            m_encodeErrors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const std::uint64_t encodeEndNs = base::monotonicNowNs();

        auto encoded = std::make_shared<base::EncodedFrame>();
        encoded->m_sequence = m_nextSequence++;
        encoded->m_captureTimestampNs = raw->m_captureTimestampNs;
        encoded->m_encodeStartTimestampNs = encodeStartNs;
        encoded->m_encodeEndTimestampNs = encodeEndNs;
        encoded->m_isKeyframe = isKeyframe;
        encoded->m_videoCodec = base::codec::VideoCodec::kH264;
        encoded->m_payload = std::move(payload);

        base::EncodedFramePtr frame = encoded;
        // 队列满时优先丢非关键帧：丢掉关键帧会让后面整个 GOP 都解不出来，
        // 所以保护它比保护积压深度更重要。
        const std::size_t dropped = m_encodedQueue.pushDropSelective(
                frame, m_config.m_encodedQueueDepth,
                [](const base::EncodedFramePtr &candidate) {
                    return candidate == nullptr || !candidate->m_isKeyframe;
                });
        if (dropped > 0) {
            m_droppedEncodedFrames.fetch_add(dropped, std::memory_order_relaxed);
            // 丢了已编码的帧意味着参考链断了，必须让编码器补一个 IDR。
            m_keyframeSignal.request();
        }
        m_encodedFrames.fetch_add(1, std::memory_order_relaxed);
    }
    m_encodedQueue.notifyAll();
}

void StreamProducer::dispatchLoop() {
    using namespace std::chrono;
    while (m_running.load(std::memory_order_acquire)) {
        base::EncodedFramePtr frame;
        if (!m_encodedQueue.waitPopFor(&frame, milliseconds(kQueueWaitMs))) {
            continue;
        }
        if (frame != nullptr && m_sink) {
            m_sink(frame);
        }
    }
    // 排空：停止时队列里还可能有已经编码好的帧，交给调用方总比直接丢弃好。
    base::EncodedFramePtr frame;
    while (m_encodedQueue.tryPop(&frame)) {
        if (frame != nullptr && m_sink) {
            m_sink(frame);
        }
    }
}

}  // namespace pipeline
}  // namespace sserver
