// ============================================================
// 文件：Renderer.h
// 模块：sclient::rendering
// 功能：渲染门面：交出可嵌入窗口的视频控件，接收解码帧并上屏。
//       公开接口不包含任何 Qt 头文件，上层（app）只依赖这一个类。
// 知识点：门面模式与 pimpl、Qt 类型不泄漏到公共接口
// ============================================================

#ifndef SCLIENT_RENDERING_RENDERER_H
#define SCLIENT_RENDERING_RENDERER_H

#include <memory>
#include <string>

#include "sclient/decoding/VideoDecoder.h"

// 前向声明 Qt 类型，避免头文件依赖 Qt 头文件
class QWidget;

namespace sclient {
namespace rendering {

// 只做"把帧画出来"，不管窗口布局与交互——那是 app 的事。
class Renderer { // 渲染门面(对外提供的简单接口)
public:
    Renderer();
    ~Renderer();

    // 不支持复制构造与赋值：每个渲染器实例只能有一个视频控件
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // 创建视频控件。真正的 GL 资源在控件首次显示时建立，所以这里不做
    // 重活，也就不会因为还没有 GL 上下文而失败。
    bool initialize(std::string *errorMessage);

    // 可嵌入窗口的控件；未 initialize 时返回 nullptr。
    QWidget *widget();

    // 提交一帧待显示图像。只在 GUI 线程调用：新旧帧之间没有队列，
    // 新帧直接覆盖未画的旧帧——实时预览只关心最新一帧。
    void submitFrame(const decoding::DecodedFrame &frame);

    // "bt601" / "bt709"：标准清晰度与高清用的 YUV→RGB 系数不同，
    // 用错会让画面偏色（肤色发绿或发紫）。
    void setColorimetry(const std::string &name); // 设置颜色空间

private:
    class Impl; // 实现类，隐藏在 pimpl 中
    std::unique_ptr<Impl> m_impl;   // pimpl 实现
   };

}  // namespace rendering
}  // namespace sclient

#endif  // SCLIENT_RENDERING_RENDERER_H
