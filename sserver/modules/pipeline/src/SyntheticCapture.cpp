// ============================================================
// 文件：SyntheticCapture.cpp
// 模块：sserver::pipeline
// 功能：替身采集源与替身编码器的实现。
// 知识点：按截止时刻节流、可预测图案、Annex B 起始码
// ============================================================

#include "sserver/pipeline/SyntheticCapture.h"

#include <chrono>
#include <thread>

#include "sserver/base/MonotonicClock.h"

namespace sserver {
namespace pipeline {

namespace {

// 假码流固定携带这么多个载荷字节：足够被切成两片以上，又不至于让
// 环回测试跑得太慢。
constexpr std::size_t kSyntheticSliceBytes = 1500;

}  // namespace

SyntheticFrameSource::SyntheticFrameSource(int width, int height, int fps)
        : m_width(width > 0 ? width : 640),
          m_height(height > 0 ? height : 480),
          m_fps(fps > 0 ? fps : 25) {
}

bool SyntheticFrameSource::start() {
    m_started = true;
    m_produced = 0;
    m_nextDeadlineNs = base::monotonicNowNs();
    return true;
}

capture::RawCaptureFramePtr SyntheticFrameSource::captureRawFrame() {
    if (!m_started) {
        return nullptr;
    }
    // 按帧间隔节流：不节流的话采集线程会把队列瞬间灌满，
    // 测出来的是丢帧而不是管线行为。
    const std::uint64_t frameIntervalNs = 1000000000ULL / static_cast<std::uint64_t>(m_fps);
    const std::uint64_t now = base::monotonicNowNs();
    if (now < m_nextDeadlineNs) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(m_nextDeadlineNs - now));
    }
    m_nextDeadlineNs += frameIntervalNs;

    auto frame = std::make_shared<base::RawVideoFrame>();
    frame->m_width = m_width;
    frame->m_height = m_height;
    frame->m_pixelFormat = base::RawVideoPixelFormat::Yuyv422;
    const std::size_t rowBytes = static_cast<std::size_t>(m_width) * 2U;
    frame->m_data.resize(rowBytes * static_cast<std::size_t>(m_height));
    frame->m_stride = rowBytes;
    frame->m_bytesUsed = frame->m_data.size();
    frame->m_captureTimestampNs = base::monotonicNowNs();

    // 可预测的测试图：亮度按行与帧号形成移动条纹，色度固定。
    // 像素值可以直接用肉眼或脚本核对，出了错很好定位。
    for (int row = 0; row < m_height; ++row) {
        std::uint8_t *rowData = frame->m_data.data() + static_cast<std::size_t>(row) * rowBytes;
        for (int column = 0; column < m_width; ++column) {
            const std::size_t offset = static_cast<std::size_t>(column) * 2U;
            const int luma = (row * 3 + column + static_cast<int>(m_produced)) & 0xFF;
            rowData[offset] = static_cast<std::uint8_t>(luma);
            rowData[offset + 1] = 128;  // U
            // 每两个像素一组：Y1 与 V 共用同一组 4 字节。
            if (column % 2 == 0) {
                rowData[offset + 2] = static_cast<std::uint8_t>(luma + 1);
                rowData[offset + 3] = 128;  // V
            }
        }
    }

    ++m_produced;
    return frame;
}

bool SyntheticEncoder::initialize(int width, int height, int fps,
                                  const appconfig::CodecConfig &config,
                                  std::string *errorMessage) {
    (void)fps;
    if (width <= 0 || height <= 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "synthetic encoder requires positive dimensions";
        }
        return false;
    }
    m_width = width;
    m_height = height;
    m_gopFrames = config.m_gopFrames > 0 ? config.m_gopFrames : 30;
    m_frameIndex = 0;
    m_forceKeyframe = true;
    m_initialized = true;
    return true;
}

bool SyntheticEncoder::encodeYuyv422Frame(const std::uint8_t *input,
                                          std::size_t inputLength,
                                          std::vector<std::uint8_t> *output,
                                          bool *isKeyframe,
                                          std::string *errorMessage) {
    if (!m_initialized || output == nullptr || isKeyframe == nullptr || input == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "synthetic encoder is not initialized";
        }
        return false;
    }

    const bool keyframe = m_forceKeyframe || (m_frameIndex % static_cast<std::uint64_t>(m_gopFrames) == 0);
    m_forceKeyframe = false;
    *isKeyframe = keyframe;

    output->clear();
    output->reserve(kSyntheticSliceBytes + 16U);

    // Annex B 起始码 + NAL 头：IDR 用 0x65、非 IDR 用 0x41（H.264 语义）。
    const std::uint8_t nalHeader = keyframe ? 0x65 : 0x41;
    output->push_back(0x00);
    output->push_back(0x00);
    output->push_back(0x00);
    output->push_back(0x01);
    output->push_back(nalHeader);

    // 帧序号写进码流，接收端可以据此确认"拼回来的确实是这一帧"。
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        output->push_back(static_cast<std::uint8_t>((m_frameIndex >> shift) & 0xFFU));
    }
    // 其余用输入像素填充，长度固定，方便断言分片数。
    const std::size_t fillBytes = kSyntheticSliceBytes - output->size();
    for (std::size_t index = 0; index < fillBytes; ++index) {
        output->push_back(input[index % inputLength]);
    }

    ++m_frameIndex;
    return true;
}

}  // namespace pipeline
}  // namespace sserver
