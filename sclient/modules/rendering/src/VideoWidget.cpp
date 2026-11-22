// ============================================================
// 文件：VideoWidget.cpp
// 模块：sclient::rendering（内部）
// 功能：GL 资源建立与逐帧绘制。YUV420P 用三个单通道纹理，NV12 用
//       单通道 + 双通道；片元着色器做颜色转换，CPU 不碰像素。
// 知识点：GL_RED 纹理、GL_UNPACK_ALIGNMENT、纹理只在尺寸变化时重分配、
//         顶点数据只建一次
// ============================================================

#include "VideoWidget.h"

#include <QSurfaceFormat>

#include <cstddef>

namespace sclient {
namespace rendering {
namespace {

// 顶点数据：位置 + 纹理坐标。纹理坐标的 v 方向是反的——GL 纹理原点在左下，
// 而我们的帧是自上而下的行序，不翻转画面会上下颠倒。
constexpr float kQuadVertices[] = {
        // x,     y,    u,    v
        -1.0f, -1.0f, 0.0f, 1.0f,   // 左下
        1.0f,  -1.0f, 1.0f, 1.0f,   // 右下
        -1.0f, 1.0f,  0.0f, 0.0f,   // 左上
        1.0f,  1.0f,  1.0f, 0.0f,   // 右上
};

// 以下两个都是着色器源码
// 顶点着色器的源码，它使用 GLSL 编写，并通过 C++11 的原始字符串字面量 R"(...)" 嵌入到了 C++ 代码中。
// 在 OpenGL 渲染管线中，这段代码会在 GPU 上运行，专门负责处理每一个顶点的数据。
// 顶点着色器：将顶点位置和纹理坐标传递给片元着色器。
// 原始字符串写法：清晰、直观,传统写法每行都要加\n
constexpr const char *kVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
uniform vec2 uScale;
out vec2 vTexCoord;
void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPosition * uScale, 0.0, 1.0);
}
)";

// 用三个 dot 代替 mat3：避免矩阵上传时的行/列主序歧义——那种错误在屏幕上
// 只表现为"颜色怪怪的"，很难查。
constexpr const char *kFragmentShader = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTextureY; // Y 通道
uniform sampler2D uTextureU; // U 通道
uniform sampler2D uTextureV; // V 通道
uniform int uIsNv12;
uniform vec3 uCoefficientR;
uniform vec3 uCoefficientG;
uniform vec3 uCoefficientB;
void main() {
    float luma = texture(uTextureY, vTexCoord).r;
    vec2 chroma;
    if (uIsNv12 == 1) {
        chroma = texture(uTextureU, vTexCoord).rg;
    } else {
        chroma = vec2(texture(uTextureU, vTexCoord).r,
                      texture(uTextureV, vTexCoord).r);
    }
    vec3 yuv = vec3(luma, chroma.x, chroma.y) - vec3(0.0625, 0.5, 0.5);
    // 矩阵乘法:rgb = mat3 * yuv
    // 点乘写法:rgb = vec3(dot(R系数, yuv), dot(G系数, yuv), dot(B系数, yuv))
    vec3 rgb = vec3(dot(uCoefficientR, yuv),
                    dot(uCoefficientG, yuv),
                    dot(uCoefficientB, yuv));
    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

}  // namespace

// 构造函数
VideoWidget::VideoWidget(QWidget *parent) : QOpenGLWidget(parent) {
    // 3.3 核心 profile：与着色器版本一致，也避免兼容模式的隐性状态。
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(0);
    format.setSwapInterval(1);
    setFormat(format);
}

VideoWidget::~VideoWidget() {
    // 从未跑过 initializeGL 的控件没有 GL 资源，也没有可用的函数指针；
    // 这时调 makeCurrent/glDelete* 会用到空指针。
    if (!m_glReady) {
        return;
    }
    // GL 资源必须在上下文仍然有效时释放：先 makeCurrent 才安全。
    makeCurrent();
    releaseTextures();
    if (m_vertexBuffer != 0U) {
        glDeleteBuffers(1, &m_vertexBuffer);
        m_vertexBuffer = 0U;
    }
    if (m_vertexArray != 0U) {
        glDeleteVertexArrays(1, &m_vertexArray);
        m_vertexArray = 0U;
    }
    doneCurrent();
}

void VideoWidget::submitFrame(const decoding::DecodedFrame &frame) {
    // 直接覆盖：积压的旧帧没有价值，显示最新一帧才是实时预览的目标。
    m_pendingFrame = frame;
    m_hasPendingFrame = true;
    update();
}

// 设置颜色空间
void VideoWidget::setColorimetry(const std::string &name) {
    if (name == "bt709") {
        m_coefficientR[0] = 1.164f; m_coefficientR[1] = 0.0f;   m_coefficientR[2] = 1.793f;
        m_coefficientG[0] = 1.164f; m_coefficientG[1] = -0.213f; m_coefficientG[2] = -0.534f;
        m_coefficientB[0] = 1.164f; m_coefficientB[1] = 2.115f; m_coefficientB[2] = 0.0f;
    } else {
        m_coefficientR[0] = 1.164f; m_coefficientR[1] = 0.0f;   m_coefficientR[2] = 1.596f;
        m_coefficientG[0] = 1.164f; m_coefficientG[1] = -0.392f; m_coefficientG[2] = -0.813f;
        m_coefficientB[0] = 1.164f; m_coefficientB[1] = 2.017f; m_coefficientB[2] = 0.0f;
    }
    update();
}

// 构建程序
bool VideoWidget::buildProgram() {
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)) {
        return false;
    }
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)) {
        return false;
    }
    return m_program.link();
}

// 初始化 GL 环境
void VideoWidget::initializeGL() {
    if (!initializeOpenGLFunctions()) {
        return;
    }
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    // 行跨距可能不是 4 的倍数（例如宽度 1610 的亮度行），按 1 字节对齐
    // 读取，否则纹理会出现斜向错位。
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (!buildProgram()) {
        return;
    }

    glGenVertexArrays(1, &m_vertexArray);
    glGenBuffers(1, &m_vertexBuffer);
    glBindVertexArray(m_vertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVertices), kQuadVertices, GL_STATIC_DRAW);

    constexpr int kStride = 4 * static_cast<int>(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride,
                          reinterpret_cast<const void *>(2 * sizeof(float)));
    glBindVertexArray(0);

    m_glReady = true;
}

// 释放纹理
void VideoWidget::releaseTextures() {
    if (m_textures[0] != 0U) {
        glDeleteTextures(3, m_textures);
        m_textures[0] = 0U;
        m_textures[1] = 0U;
        m_textures[2] = 0U;
    }
    m_texturesReady = false;
}

// 只在尺寸或格式变化时调用：重新分配纹理对象与存储。
void VideoWidget::ensureTextures(const decoding::DecodedFrame &frame) {
    releaseTextures();
    glGenTextures(3, m_textures);
    for (unsigned int &texture : m_textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    const int chromaWidth = frame.m_width / 2;
    const int chromaHeight = frame.m_height / 2;

    // glBindTexture: 绑定纹理对象到当前上下文，此处绑定亮度纹理 Y
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, frame.m_width, frame.m_height, 0,
                 GL_RED, GL_UNSIGNED_BYTE, nullptr);

    // 根据像素格式分配色度（Chroma）纹理：
    // NV12 的 UV 是交织存储的（半宽半高的单平面），用双通道 RG8 纹理装；
    // I420 等其他格式的 U/V 是分离存储的（两个独立平面），各用单通道 R8 纹理装。
    if (frame.m_format == decoding::DecodedPixelFormat::Nv12) {
        // NV12：UV 交织在一个平面里，绑定到 m_textures[1]，
        // 内部格式 GL_RG8 / 像素格式 GL_RG，每个像素含 (U, V) 两个分量。
        glBindTexture(GL_TEXTURE_2D, m_textures[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, chromaWidth, chromaHeight, 0,
                     GL_RG, GL_UNSIGNED_BYTE, nullptr);
    } else { 
        // I420（或其他平面格式）：U、V 各自独立成平面，
        // 分别绑定到 m_textures[1] 和 m_textures[2]，
        // 内部格式 GL_R8 / 像素格式 GL_RED，每个像素只含一个分量。
        glBindTexture(GL_TEXTURE_2D, m_textures[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, chromaWidth, chromaHeight, 0,
                     GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, m_textures[2]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, chromaWidth, chromaHeight, 0,
                     GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }

    m_textureWidth = frame.m_width;
    m_textureHeight = frame.m_height;
    m_textureFormat = frame.m_format;
    m_texturesReady = true;
}

void VideoWidget::paintGL() {
    if (!m_glReady) {
        return;
    }
    glClear(GL_COLOR_BUFFER_BIT);

    if (!m_hasPendingFrame) {
        return;
    }

    const decoding::DecodedFrame &frame = m_pendingFrame;
    const bool needsAllocation = !m_texturesReady
            || m_textureWidth != frame.m_width
            || m_textureHeight != frame.m_height
            || m_textureFormat != frame.m_format;
    if (needsAllocation) {
        ensureTextures(frame);
    }

    // 每帧只更新内容，不重建纹理对象：每帧创建/销毁 GPU 对象既慢又容易
    // 留下状态泄漏。
    const int chromaWidth = frame.m_width / 2;
    const int chromaHeight = frame.m_height / 2;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.m_width, frame.m_height, GL_RED,
                    GL_UNSIGNED_BYTE, frame.m_y.data());

    const bool nv12 = frame.m_format == decoding::DecodedPixelFormat::Nv12;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    if (nv12) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chromaWidth, chromaHeight, GL_RG,
                        GL_UNSIGNED_BYTE, frame.m_u.data());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chromaWidth, chromaHeight, GL_RED,
                        GL_UNSIGNED_BYTE, frame.m_u.data());
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_textures[2]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chromaWidth, chromaHeight, GL_RED,
                        GL_UNSIGNED_BYTE, frame.m_v.data());
    }

    m_frameIsNv12 = nv12;
    updateAspectScale();
    m_hasPendingFrame = false;

    m_program.bind();
    m_program.setUniformValue("uScale", m_scaleX, m_scaleY);
    m_program.setUniformValue("uTextureY", 0);
    m_program.setUniformValue("uTextureU", 1);
    m_program.setUniformValue("uTextureV", 2);
    m_program.setUniformValue("uIsNv12", m_frameIsNv12 ? 1 : 0);
    m_program.setUniformValue("uCoefficientR", m_coefficientR[0], m_coefficientR[1],
                              m_coefficientR[2]);
    m_program.setUniformValue("uCoefficientG", m_coefficientG[0], m_coefficientG[1],
                              m_coefficientG[2]);
    m_program.setUniformValue("uCoefficientB", m_coefficientB[0], m_coefficientB[1],
                              m_coefficientB[2]);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textures[2]);

    glBindVertexArray(m_vertexArray);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    m_program.release();
}

void VideoWidget::resizeGL(int, int) {
    // 宽高比在 paintGL 里按"控件尺寸 / 帧尺寸"重算：resize 时可能还没有帧，
    // 也可能帧尺寸刚变过，集中在一处算更不容易漏。
    update();
}

void VideoWidget::updateAspectScale() {
    const float frameAspect = static_cast<float>(m_textureWidth)
            / static_cast<float>(m_textureHeight > 0 ? m_textureHeight : 1);
    const float widgetAspect = static_cast<float>(width())
            / static_cast<float>(height() > 0 ? height() : 1);
    if (widgetAspect > frameAspect) {
        // 控件更宽：左右留黑边，画面按高度铺满。
        m_scaleX = frameAspect / widgetAspect;
        m_scaleY = 1.0f;
    } else {
        m_scaleX = 1.0f;
        m_scaleY = widgetAspect / frameAspect;
    }
}

}  // namespace rendering
}  // namespace sclient
