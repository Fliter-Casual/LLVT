// StreamProducer：三级流水线的端到端测试。
//
// 用假帧源（可预测像素）和假编码器（把输入前 16 字节当码流）跑真实的三条
// 线程与真实队列，验证顺序、计数与停止排空。真实编码器/采集器接入后，
// 这些结构不应该有任何改动。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sserver/appconfig/CodecConfig.h"
#include "sserver/base/MonotonicClock.h"
#include "sserver/capture/FrameSource.h"
#include "sserver/encoding/Encoder.h"
#include "sserver/pipeline/StreamProducer.h"
#include "support/Check.h"

namespace {

using sserver::pipeline::StreamProducer;
using sserver::pipeline::StreamProducerConfig;

// 逐帧产出的假采集源：每 2ms 一帧，产出 totalFrames 帧后返回 nullptr。
class FakeFrameSource : public sserver::capture::FrameSource {
public:
    FakeFrameSource(int totalFrames, int width, int height, int fps)
            : m_totalFrames(totalFrames), m_width(width), m_height(height), m_fps(fps) {
    }

    bool open() override { return m_openSucceeds; }
    bool start() override { return true; }
    int effectiveWidth() const override { return m_width; }
    int effectiveHeight() const override { return m_height; }
    int effectiveFps() const override { return m_fps; }
    std::string effectivePixelFormat() const override { return "yuyv"; }

    sserver::capture::RawCaptureFramePtr captureRawFrame() override {
        if (m_produced >= m_totalFrames) {
            // 没有帧时也要有节奏地让出 CPU，模拟驱动的超时窗口。
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return nullptr;
        }
        auto frame = std::make_shared<sserver::base::RawVideoFrame>();
        frame->m_width = m_width;
        frame->m_height = m_height;
        frame->m_pixelFormat = sserver::base::RawVideoPixelFormat::Yuyv422;
        const std::size_t bytes =
                static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height) * 2U;
        // 像素值带帧序号，便于核对"收到的确实是这一帧"。
        frame->m_data.assign(bytes, static_cast<std::uint8_t>(m_produced & 0xFF));
        frame->m_bytesUsed = bytes;
        frame->m_stride = static_cast<std::size_t>(m_width) * 2U;
        frame->m_captureTimestampNs = sserver::base::monotonicNowNs();
        ++m_produced;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return frame;
    }

    void stop() override { m_stopped = true; }
    void close() override { m_closed = true; }
    std::string describe() const override { return "fake-source"; }

    bool m_openSucceeds = true;
    int m_produced = 0;
    bool m_stopped = false;
    bool m_closed = false;

private:
    int m_totalFrames;
    int m_width;
    int m_height;
    int m_fps;
};

// 假编码器：码流取输入前 16 字节；每 5 帧出一个关键帧；forceKeyframe 让
// 下一帧变成关键帧。
class FakeEncoder : public sserver::encoding::Encoder {
public:
    bool initialize(int width, int height, int fps,
                    const sserver::appconfig::CodecConfig &, std::string *) override {
        m_width = width;
        m_height = height;
        m_fps = fps;
        return true;
    }

    void forceKeyframe() override {
        m_forceKeyframeCalls.fetch_add(1);
        m_pendingKeyframe.store(true);
    }

    bool encodeYuyv422Frame(const std::uint8_t *input, std::size_t inputLength,
                            std::vector<std::uint8_t> *output, bool *isKeyframe,
                            std::string *) override {
        const int call = m_encodedCalls.fetch_add(1);
        const bool periodicKeyframe = (call % 5 == 0);
        *isKeyframe = periodicKeyframe || m_pendingKeyframe.exchange(false);
        const std::size_t copyBytes = std::min<std::size_t>(inputLength, 16);
        output->assign(input, input + copyBytes);
        return true;
    }

    void shutdown() override { m_shutdownCalls.fetch_add(1); }

    std::atomic<int> m_encodedCalls{0};
    std::atomic<int> m_forceKeyframeCalls{0};
    std::atomic<int> m_shutdownCalls{0};
    std::atomic<bool> m_pendingKeyframe{false};
    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;
};

// 等到谓词成立或超时；返回值只用于断言，不参与控制流。
template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

void testFramesFlowThroughPipeline() {
    std::mutex mutex;
    std::vector<sserver::base::EncodedFramePtr> received;
    const auto sink = [&mutex, &received](sserver::base::EncodedFramePtr frame) {
        std::lock_guard<std::mutex> lock(mutex);
        received.push_back(std::move(frame));
    };

    auto source = std::make_unique<FakeFrameSource>(30, 320, 240, 25);
    FakeFrameSource *sourcePtr = source.get();
    auto encoder = std::make_unique<FakeEncoder>();
    FakeEncoder *encoderPtr = encoder.get();

    StreamProducerConfig config;
    config.m_rawQueueDepth = 8;  // 测试里给足余量，避免与产出节奏抢跑
    StreamProducer producer(std::move(source), std::move(encoder),
                            sserver::appconfig::CodecConfig{}, config);

    std::string error;
    CHECK(producer.start(sink, &error));
    CHECK(producer.running());

    const bool arrived = waitFor([&mutex, &received]() {
        std::lock_guard<std::mutex> lock(mutex);
        return received.size() >= 30;
    });
    producer.stop();
    CHECK(arrived);
    CHECK(!producer.running());

    // 尺寸与帧率必须来自采集源上报的实际值，而不是硬编码的兜底值。
    CHECK(producer.width() == 320);
    CHECK(producer.height() == 240);
    CHECK(producer.fps() == 25);
    CHECK(encoderPtr->m_width == 320);
    CHECK(encoderPtr->m_fps == 25);

    std::lock_guard<std::mutex> lock(mutex);
    CHECK(received.size() == 30);
    for (std::size_t index = 0; index < received.size(); ++index) {
        const auto &frame = received[index];
        CHECK(frame != nullptr);
        // 序号必须严格连续递增：断号意味着中间被丢帧。
        CHECK(frame->m_sequence == index);
        // 编码输出就是输入前 16 字节，首字节是源帧序号。
        CHECK(frame->m_payload.size() == 16);
        CHECK(frame->m_payload[0] == static_cast<std::uint8_t>(index & 0xFF));
        // 时间戳必须来自真实打点，不能是 0。
        CHECK(frame->m_captureTimestampNs > 0);
        CHECK(frame->m_encodeEndTimestampNs >= frame->m_encodeStartTimestampNs);
    }
    // 每 5 帧一个关键帧。
    CHECK(received[0]->m_isKeyframe);
    CHECK(received[5]->m_isKeyframe);
    CHECK(!received[1]->m_isKeyframe);

    const StreamProducer::Stats stats = producer.stats();
    CHECK(stats.m_encodedFrames == 30);
    CHECK(stats.m_encodeErrors == 0);
    CHECK(encoderPtr->m_shutdownCalls == 1);
    CHECK(sourcePtr->m_stopped);
    CHECK(sourcePtr->m_closed);
}

void testKeyframeRequestReachesEncoder() {
    std::mutex mutex;
    std::size_t count = 0;
    const auto sink = [&mutex, &count](sserver::base::EncodedFramePtr) {
        std::lock_guard<std::mutex> lock(mutex);
        ++count;
    };

    auto source = std::make_unique<FakeFrameSource>(1000, 160, 120, 30);
    auto encoder = std::make_unique<FakeEncoder>();
    FakeEncoder *encoderPtr = encoder.get();
    StreamProducerConfig config;
    config.m_rawQueueDepth = 8;
    StreamProducer producer(std::move(source), std::move(encoder),
                            sserver::appconfig::CodecConfig{}, config);

    std::string error;
    CHECK(producer.start(sink, &error));
    CHECK(waitFor([&mutex, &count]() {
        std::lock_guard<std::mutex> lock(mutex);
        return count >= 5;
    }));

    // 正常情况下编码器不会收到强制关键帧请求。
    CHECK(encoderPtr->m_forceKeyframeCalls.load() == 0);
    producer.requestKeyframe();
    CHECK(waitFor([encoderPtr]() { return encoderPtr->m_forceKeyframeCalls.load() >= 1; }, 2000));

    producer.stop();
}

void testStartValidation() {
    StreamProducerConfig config;
    std::string error;

    // 没有 sink。
    {
        StreamProducer producer(std::make_unique<FakeFrameSource>(1, 64, 64, 30),
                                std::make_unique<FakeEncoder>(),
                                sserver::appconfig::CodecConfig{}, config);
        CHECK(!producer.start(sserver::pipeline::EncodedFrameSink(), &error));
        CHECK(error.find("sink") != std::string::npos);
    }
    // 队列深度为 0。
    {
        StreamProducerConfig badConfig;
        badConfig.m_rawQueueDepth = 0;
        StreamProducer producer(std::make_unique<FakeFrameSource>(1, 64, 64, 30),
                                std::make_unique<FakeEncoder>(),
                                sserver::appconfig::CodecConfig{}, badConfig);
        CHECK(!producer.start([](sserver::base::EncodedFramePtr) {}, &error));
        CHECK(error.find("queue depths") != std::string::npos);
    }
    // 采集源打不开。
    {
        auto source = std::make_unique<FakeFrameSource>(1, 64, 64, 30);
        source->m_openSucceeds = false;
        StreamProducer producer(std::move(source), std::make_unique<FakeEncoder>(),
                                sserver::appconfig::CodecConfig{}, config);
        CHECK(!producer.start([](sserver::base::EncodedFramePtr) {}, &error));
        CHECK(error.find("open frame source") != std::string::npos);
    }
    // 重复启动。
    {
        StreamProducer producer(std::make_unique<FakeFrameSource>(1000, 64, 64, 30),
                                std::make_unique<FakeEncoder>(),
                                sserver::appconfig::CodecConfig{}, config);
        CHECK(producer.start([](sserver::base::EncodedFramePtr) {}, &error));
        CHECK(!producer.start([](sserver::base::EncodedFramePtr) {}, &error));
        CHECK(error.find("already running") != std::string::npos);
        producer.stop();
    }
}

void testStopIsIdempotent() {
    // 从未启动就 stop 不能崩，也不能留下未 join 的线程。
    {
        StreamProducer producer(std::make_unique<FakeFrameSource>(1, 64, 64, 30),
                                std::make_unique<FakeEncoder>(),
                                sserver::appconfig::CodecConfig{});
        producer.stop();
        producer.stop();
    }
    // 启动后再连停两次。
    {
        StreamProducer producer(std::make_unique<FakeFrameSource>(1000, 64, 64, 30),
                                std::make_unique<FakeEncoder>(),
                                sserver::appconfig::CodecConfig{});
        std::string error;
        CHECK(producer.start([](sserver::base::EncodedFramePtr) {}, &error));
        producer.stop();
        producer.stop();
        CHECK(!producer.running());
    }
}

}  // namespace

int main() {
    testFramesFlowThroughPipeline();
    testKeyframeRequestReachesEncoder();
    testStartValidation();
    testStopIsIdempotent();
    return sserver::test::finish("stream_producer_test");
}
