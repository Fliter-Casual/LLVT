# encoding：采集帧到 H.264 的编码。
#
# libx264 是可选依赖：装上就用真编码器，没有就编译失败实现并在启动时告警。
# 这样开发机（没装库）与目标机（装了库）都能构建，且失败模式是明确的。
add_library(sserver_encoding STATIC
    src/I420Convert.cpp
    src/X264Encoder.cpp
)
add_library(sserver::encoding ALIAS sserver_encoding)
target_include_directories(sserver_encoding PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Encoder.h 的接口里出现 appconfig::CodecConfig，所以这是公有依赖。
target_link_libraries(sserver_encoding PUBLIC sserver::base sserver::appconfig)
sserver_set_warnings(sserver_encoding)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(X264 QUIET x264)
endif()
if(X264_FOUND)
    message(STATUS "encoding: 使用 libx264 ${X264_VERSION}")
    target_compile_definitions(sserver_encoding PRIVATE SSERVER_HAS_X264)
    target_include_directories(sserver_encoding PRIVATE ${X264_INCLUDE_DIRS})
    target_link_directories(sserver_encoding PRIVATE ${X264_LIBRARY_DIRS})
    target_link_libraries(sserver_encoding PRIVATE ${X264_LIBRARIES})
else()
    message(STATUS "encoding: 未找到 libx264 —— 将回退合成编码器（码流不可解码）")
endif()