// ============================================================
// 文件：SyntheticCapture.h
// 模块：sserver::pipeline
// 功能：开发与联调用的替身采集源与替身编码器：按帧率生成可预测的 YUYV
//       测试图，并产出带 Annex B 起始码的假码流。
// 知识点：工厂替身、可预测测试图、按时间片节流
// ============================================================

#ifndef SSERVER_PIPELINE_SYNTHETIC_CAPTURE_H
#define SSERVER_PIPELINE_SYNTHETIC_CAPTURE_H

#include <cstdint>
#include <vector>

#include "sserver/appconfig/CodecConfig.h"
#include "sserver/capture/FrameSource.h"
#include "sserver/encoding/Encoder.h"

namespace sserver {
namespace pipeline {

// 为什么要有它：整条管线（线程、队列、分片、控制面）都能在没有摄像头和
// 硬件编码器的机器上验证，真机上就只剩"接上 V4L2 与 x264"这一件事。
// 注意它不是生产路径：main 里用 --synthetic 显式开启。
class SyntheticFrameSource : public capture::FrameSource {
public:
    SyntheticFrameSource(int width, int height, int fps);

    bool open() override { return true; }
    bool start() override;
    int effectiveWidth() const override { return m_width; }
    int effectiveHeight() const override { return m_height; }
    int effectiveFps() const override { return m_fps; }
    std::string effectivePixelFormat() const override { return "yuyv"; }

    capture::RawCaptureFramePtr captureRawFrame() override;
    void stop() override { m_started = false; }
    void close() override {}

    std::string describe() const override { return "synthetic"; }

    std::uint64_t producedFrames() const { return m_produced; }

private:
    int m_width;
    int m_height;
    int m_fps;
    bool m_started = false;
    std::uint64_t m_produced = 0;
    std::uint64_t m_nextDeadlineNs = 0;
};

// 占位编码器：产出 "起始码 + NAL 头 + 帧序号 + 固定长度填充" 的假码流。
// 它不是合法 H.264，客户端这时候还解不了——这一步只验证分片、传输与
// 重组；接上 x264 后这个类会被删除。
class SyntheticEncoder : public encoding::Encoder {
public:
    bool initialize(int width, int height, int fps,
                    const appconfig::CodecConfig &config,
                    std::string *errorMessage) override;

    void forceKeyframe() override { m_forceKeyframe = true; }

    bool encodeYuyv422Frame(const std::uint8_t *input,
                            std::size_t inputLength,
                            std::vector<std::uint8_t> *output,
                            bool *isKeyframe,
                            std::string *errorMessage) override;

    void shutdown() override { m_initialized = false; }

private:
    bool m_initialized = false;
    bool m_forceKeyframe = true;   // 第一帧必须是关键帧
    int m_width = 0;
    int m_height = 0;
    int m_gopFrames = 30;
    std::uint64_t m_frameIndex = 0;
};

}  // namespace pipeline
}  // namespace sserver

#endif  // SSERVER_PIPELINE_SYNTHETIC_CAPTURE_H
