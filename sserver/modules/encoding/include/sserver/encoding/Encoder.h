// ============================================================
// 文件：Encoder.h
// 模块：sserver::encoding
// 功能：视频编码器的抽象接口：initialize 一次配置，之后逐帧 encode，
//       shutdown 释放；x264 软编与 MPP 硬编各实现一份。
// 知识点：纯虚接口、虚函数默认适配、输出参数回传错误
// ============================================================

#ifndef SSERVER_ENCODING_ENCODER_H
#define SSERVER_ENCODING_ENCODER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sserver/appconfig/CodecConfig.h"
#include "sserver/base/RawVideoFrame.h"

namespace sserver {
namespace encoding {

// 契约：initialize 成功后可以连续编码，直到 shutdown；
// forceKeyframe / setBitrate 可在对象存活期间由别的线程调用；
// initialize 与 shutdown 不得与编码并发；同一实例的 encode 由单个编码
// 线程驱动（所以编码器内部不需要为 encode 加锁）。
class Encoder {
public:
    virtual ~Encoder() = default;

    virtual bool initialize(int width, int height, int fps,
                            const appconfig::CodecConfig &config,
                            std::string *errorMessage) = 0;

    // 请求下一帧编成 IDR。线程安全：实现内部只置一个原子标志，
    // 由编码线程在帧边界取走。
    virtual void forceKeyframe() {}

    // 运行时码率调整。线程安全，同样只记录挂起请求，帧边界应用。
    virtual void setBitrate(std::int64_t bitrateBps) { (void)bitrateBps; }

    // 编码一帧紧密打包的 YUYV422（stride = width*2）。
    virtual bool encodeYuyv422Frame(const std::uint8_t *input,
                                    std::size_t inputLength,
                                    std::vector<std::uint8_t> *output,
                                    bool *isKeyframe,
                                    std::string *errorMessage) = 0;

    // 按视图编码。基类默认只接受 CPU 内存的紧凑 YUYV422，并把调用转给
    // encodeYuyv422Frame；支持 MJPEG、DMA-BUF 或任意行距的后端应重写它。
    virtual bool encodeFrame(const base::RawVideoFrameView &frame,
                             std::vector<std::uint8_t> *output,
                             bool *isKeyframe,
                             std::string *errorMessage) {
        if (frame.m_pixelFormat != base::RawVideoPixelFormat::Yuyv422
            || frame.m_data == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = "encoder backend only accepts CPU-backed YUYV422 frames";
            }
            return false;
        }
        return encodeYuyv422Frame(frame.m_data, frame.m_bytesUsed, output, isKeyframe,
                                  errorMessage);
    }

    virtual void shutdown() = 0;
};

}  // namespace encoding
}  // namespace sserver

#endif  // SSERVER_ENCODING_ENCODER_H
