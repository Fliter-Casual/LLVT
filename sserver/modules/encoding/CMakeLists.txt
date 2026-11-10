# encoding：采集帧到 H.264/H.265 的编码。
#
# 当前只有 YUYV→I420 的色彩转换（纯标量实现）。x264/MPP 两个后端要等
# 管线能跑起来再接；NEON 版本等有板子可以对照标量结果时再加——
# 未经真机验证的 SIMD 代码很容易带来肉眼才看得出来的偏色。
add_library(sserver_encoding STATIC
    src/I420Convert.cpp
)
add_library(sserver::encoding ALIAS sserver_encoding)
target_include_directories(sserver_encoding PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sserver_encoding PUBLIC sserver::base)
sserver_set_warnings(sserver_encoding)