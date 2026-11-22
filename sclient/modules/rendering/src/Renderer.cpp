// ============================================================
// 文件：Renderer.cpp
// 模块：sclient::rendering
// 功能：渲染门面的实现：持有视频控件，把 submitFrame 转给控件。
// 知识点：pimpl 隔离 Qt 类型、控件生命周期归 门面(对外提供一个最简单的接口) 所有
// ============================================================

#include "sclient/rendering/Renderer.h"

#include "VideoWidget.h"

namespace sclient {
namespace rendering {

// 渲染门面的实现类：持有视频控件，把 submitFrame 转给控件。
class Renderer::Impl {
public:
    VideoWidget *m_widget = nullptr;
};

// 渲染门面的构造函数：创建视频控件。
Renderer::Renderer() : m_impl(new Impl()) {}

Renderer::~Renderer() {
    // 控件由 Qt 的父子关系或窗口负责销毁；这里只放弃引用。
    m_impl->m_widget = nullptr;
}

bool Renderer::initialize(std::string *errorMessage) {
    if (m_impl->m_widget != nullptr) {
        return true;
    }
    m_impl->m_widget = new VideoWidget();
    if (m_impl->m_widget == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "failed to create the video widget";
        }
        return false;
    }
    return true;
}

// 渲染门面的 widget 函数：返回视频控件。
QWidget *Renderer::widget() {
    return m_impl->m_widget;
}

void Renderer::submitFrame(const decoding::DecodedFrame &frame) {
    if (m_impl->m_widget != nullptr) {
        m_impl->m_widget->submitFrame(frame);
    }
}

void Renderer::setColorimetry(const std::string &name) {
    if (m_impl->m_widget != nullptr) {
        m_impl->m_widget->setColorimetry(name);
    }
}

}  // namespace rendering
}  // namespace sclient
