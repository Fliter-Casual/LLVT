// ============================================================
// 文件：VideoWidget.h
// 模块：sclient::rendering（内部）
// 功能：QOpenGLWidget 子类：接收解码帧，把 YUV 平面上传成纹理，在片元
//       着色器里做 YUV→RGB 与宽高比适配后绘制。
// 知识点：QOpenGLWidget 生命周期（initializeGL/paintGL/resizeGL）、
//         QOpenGLFunctions_3_3_Core、GL_RED 单通道纹理、GL_UNPACK_ALIGNMENT
// ============================================================

#ifndef SCLIENT_RENDERING_VIDEO_WIDGET_H
#define SCLIENT_RENDERING_VIDEO_WIDGET_H

// 视频控件的头文件：定义了视频控件的类，包括初始化、绘制、调整大小等。
#include <QOpenGLFunctions_3_3_Core> // OpenGL 函数，用于绘制视频帧
#include <QOpenGLShaderProgram> // 着色器程序，用于绘制视频帧
#include <QOpenGLWidget> // OpenGL 控件，用于绘制视频帧

#include <string>
#include <vector>

#include "sclient/decoding/VideoDecoder.h"

namespace sclient {
namespace rendering {

// 视频控件的实现类：接收解码帧，把 YUV 平面上传成纹理，在片元着色器里做 YUV→RGB 与宽高比适配后绘制
// 继承 QOpenGLWidget：提供 OpenGL 控件的生命周期（initializeGL/paintGL/resizeGL）
// 继承 QOpenGLFunctions_3_3_Core：提供 OpenGL 函数（glTexImage2D、glTexImage2D、glTexImage2D）
class VideoWidget final : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
public:
    explicit VideoWidget(QWidget *parent = nullptr);
    ~VideoWidget() override;

    // 只在 GUI 线程调用。新帧覆盖未画的旧帧：实时预览要最新，不排队。
    void submitFrame(const decoding::DecodedFrame &frame);

    // 色彩空间标准:决定了“YUV 数据如何转换成 RGB 颜色:
    //"bt601" / "bt709"；其他值按 bt601 处理。
    //"bt601"：标清（SD）时代的色彩标准（比如 480p、576p 的 DVD 和老式电视）
    //"bt709"：高清（HD）时代的色彩标准（比如 720p、1080p 的蓝光、网络视频）
    void setColorimetry(const std::string &name); // 设置色彩空间标准

protected:
    void initializeGL() override; // 初始化 OpenGL 环境
    void paintGL() override; // 绘制视频帧
    void resizeGL(int width, int height) override; // 调整视频控件大小

private:
    bool buildProgram(); // 构建着色器程序
    void releaseTextures(); // 释放纹理
    void ensureTextures(const decoding::DecodedFrame &frame); // 确保纹理
    void updateAspectScale(); // 更新宽高比适配缩放因子

    QOpenGLShaderProgram m_program; // 着色器程序(调用GPU渲染)，用于绘制视频帧
    unsigned int m_textures[3] = {0U, 0U, 0U}; // 3 个纹理，用于存储 YUV 平面
    unsigned int m_vertexArray = 0U; // 顶点数组，用于存储视频帧的顶点数据
    unsigned int m_vertexBuffer = 0U; // 顶点缓冲区，用于存储视频帧的顶点数据
    bool m_glReady = false; // OpenGL 环境是否初始化完成
    bool m_texturesReady = false; // 纹理是否初始化完成

    // 纹理尺寸/格式变化时才重新分配；其余帧只做 subimage 更新。
    int m_textureWidth = 0; // 纹理宽度
    int m_textureHeight = 0; // 纹理高度
    decoding::DecodedPixelFormat m_textureFormat =
            decoding::DecodedPixelFormat::Unsupported; // 纹理格式
    bool m_frameIsNv12 = false; // 是否是 NV12 格式的帧

    bool m_hasPendingFrame = false; // 是否有待处理帧
    decoding::DecodedFrame m_pendingFrame; // 待处理帧

    // YUV→RGB 系数（limited range：Y 黑电平 16，色度中点 128）。
    float m_coefficientR[3] = {1.164f, 0.0f, 1.596f}; // 红色系数
    float m_coefficientG[3] = {1.164f, -0.392f, -0.813f}; // 绿色系数
    float m_coefficientB[3] = {1.164f, 2.017f, 0.0f}; // 蓝色系数

    float m_scaleX = 1.0f; // 宽度缩放因子
    float m_scaleY = 1.0f; // 高度缩放因子
};

}  // namespace rendering
}  // namespace sclient

#endif  // SCLIENT_RENDERING_VIDEO_WIDGET_H
