cmake_minimum_required(VERSION 3.16) # 最低版本要求

project(sclient
    VERSION 0.1.0
    DESCRIPTION "低延时视频图传系统 - 客户端"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17) # C++17 标准
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 严格要求 C++17 标准
set(CMAKE_CXX_EXTENSIONS OFF) # 不允许使用非标准扩展

# 模块按依赖自底向上添加：base（帧模型/队列） → logger（日志） → decoding（FFmpeg 解码） → rendering（OpenGL） → transport（收流）
#                → mediasec（加解密） → recording（MP4 录像）→ control（控制协议） → ui（Qt 界面） → cli（参数解析） → playback（播放编排） → app（入口）
# 骨架阶段暂无模块，随后续开发逐个 add_subdirectory。


