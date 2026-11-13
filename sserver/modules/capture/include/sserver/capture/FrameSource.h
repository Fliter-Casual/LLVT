// ============================================================
// 文件：FrameSource.h
// 模块：sserver::capture
// 功能：原始视频采集源的抽象接口，约定 open → start → 循环 captureRawFrame
//       → stop → close 的生命周期，供 V4L2 等实现接入流水线。
// 知识点：纯虚接口与虚析构、生命周期契约、多态替换实现
// ============================================================

#ifndef SSERVER_CAPTURE_FRAME_SOURCE_H
#define SSERVER_CAPTURE_FRAME_SOURCE_H

#include <memory>
#include <string>

#include "sserver/base/RawVideoFrame.h"

namespace sserver {
namespace capture {

using RawCaptureFrame = base::RawVideoFrame;
using RawCaptureFramePtr = base::RawVideoFramePtr;

// 只约定生命周期，不携带实现状态：新增采集后端时实现本接口即可接入
// 流水线，上层代码不用改。pipeline 通过工厂注入实现，因此测试可以在
// 没有摄像头的机器上跑完整条管线。
class FrameSource {
public:
    virtual ~FrameSource() = default;

    virtual bool open() = 0;
    virtual bool start() = 0;

    // start() 之后可用；返回 0 表示"沿用请求尺寸"。
    virtual int effectiveWidth() const { return 0; }
    virtual int effectiveHeight() const { return 0; }
    // start() 之后可用：驱动协商出来的实际帧率可能不等于请求值，
    // 而码控参数是按帧率换算每帧预算的，所以必须以实际值为准。
    virtual int effectiveFps() const { return 0; }
    virtual std::string effectivePixelFormat() const { return {}; }

    // 取一帧。返回 nullptr 表示本轮没有帧（超时或暂时性错误），
    // 调用方应重试而不是当成致命错误——刚开流的窗口期就是这种状态。
    virtual RawCaptureFramePtr captureRawFrame() = 0;

    virtual void stop() = 0;
    virtual void close() = 0;
    virtual std::string describe() const = 0;
};

}  // namespace capture
}  // namespace sserver

#endif  // SSERVER_CAPTURE_FRAME_SOURCE_H
